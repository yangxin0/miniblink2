#include "miniblink_host/session/mb_session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <string>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "base/unguessable_token.h"
#include "miniblink_host/frame/mb_dom_storage.h"
#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {

namespace {
constexpr char kProfileIdFile[] = "profile.id";
constexpr char kProfileIdMagicV1[] = "MBPROFILE1\n";
constexpr char kProfileIdMagicV2[] = "MBPROFILE2\n";

// Several handles may intentionally open the same persistent profile. The
// manifest id makes their in-memory stores shared, so restore once and keep the
// migration/backup guard until the final handle closes.
struct ProfileState {
  size_t handles = 0;
  bool restored = false;
  bool idb_flush_safe = true;
  bool opfs_flush_safe = true;
  bool idb_needs_backup = false;
  bool opfs_needs_backup = false;
  bool idb_backup_complete = false;
  bool opfs_backup_complete = false;
};

std::map<std::string, ProfileState>& ProfileStates() {
  static auto* states = new std::map<std::string, ProfileState>();
  return *states;
}

ProfileState& RegisterProfile(const std::string& id) {
  ProfileState& state = ProfileStates()[id];
  ++state.handles;
  return state;
}

ProfileState* FindProfile(const std::string& id) {
  auto& states = ProfileStates();
  auto it = states.find(id);
  return it == states.end() ? nullptr : &it->second;
}

void UnregisterProfile(const std::string& id) {
  auto& states = ProfileStates();
  auto it = states.find(id);
  if (it == states.end())
    return;
  if (it->second.handles > 1) {
    --it->second.handles;
    return;
  }
  states.erase(it);
}

bool IsProfileToken(const std::string& token) {
  return token.size() == 32 &&
         std::all_of(token.begin(), token.end(), [](unsigned char c) {
           return std::isxdigit(c) != 0;
         });
}

std::string HexEncode(const std::string& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    encoded.push_back(kHex[byte >> 4]);
    encoded.push_back(kHex[byte & 0x0f]);
  }
  return encoded;
}

std::string ProfileBinding(const base::FilePath& profile_dir) {
  // The marker is bound to the resolved FULL directory, not the caller's path
  // spelling. Symlink/case/normalization aliases resolve to the same binding;
  // copying the directory elsewhere does not.
  return HexEncode(profile_dir.AsUTF8Unsafe());
}

std::string ProfileIdContents(const std::string& token,
                              const std::string& binding) {
  return std::string(kProfileIdMagicV2) + token + "\n" + binding + "\n";
}

bool ParseProfileId(const std::string& contents,
                    const std::string& expected_binding,
                    std::string* out,
                    bool* needs_rebind) {
  if (needs_rebind)
    *needs_rebind = false;
  if (contents.rfind(kProfileIdMagicV1, 0) == 0) {
    std::string token = contents.substr(sizeof(kProfileIdMagicV1) - 1);
    if (!token.empty() && token.back() == '\n')
      token.pop_back();
    if (!IsProfileToken(token))
      return false;
    *out = std::move(token);
    if (needs_rebind)
      *needs_rebind = true;  // v1 did not identify the physical directory
    return true;
  }
  if (contents.rfind(kProfileIdMagicV2, 0) != 0)
    return false;

  const size_t token_start = sizeof(kProfileIdMagicV2) - 1;
  const size_t token_end = contents.find('\n', token_start);
  if (token_end == std::string::npos)
    return false;
  std::string token = contents.substr(token_start, token_end - token_start);
  size_t binding_end = contents.find('\n', token_end + 1);
  if (binding_end == std::string::npos)
    binding_end = contents.size();
  if (binding_end != contents.size() && binding_end + 1 != contents.size())
    return false;
  const std::string binding =
      contents.substr(token_end + 1, binding_end - token_end - 1);
  if (!IsProfileToken(token) || binding.empty())
    return false;
  *out = std::move(token);
  if (needs_rebind)
    *needs_rebind = binding != expected_binding;
  return true;
}

bool ReadProfileId(const base::FilePath& profile_dir, std::string* out) {
  std::string contents;
  bool needs_rebind = false;
  return base::ReadFileToString(profile_dir.AppendASCII(kProfileIdFile),
                                &contents) &&
         ParseProfileId(contents, ProfileBinding(profile_dir), out,
                        &needs_rebind) &&
         !needs_rebind;
}

bool ReplaceProfileId(const base::FilePath& profile_dir,
                      const std::string& contents) {
  base::FilePath temp;
  if (!base::CreateTemporaryFileInDir(profile_dir, &temp))
    return false;
  const bool ok = base::WriteFile(temp, contents) &&
                  base::ReplaceFile(temp,
                                    profile_dir.AppendASCII(kProfileIdFile),
                                    nullptr);
  if (!ok)
    base::DeleteFile(temp);
  return ok;
}

// The marker is created exclusively. Another process can win the race, but it
// cannot be overwritten with a different identity. A corrupt/partial existing
// marker is never replaced automatically because that could strand snapshots
// already keyed to it.
bool LoadOrCreateProfileId(const base::FilePath& profile_dir,
                           std::string* out) {
  const base::FilePath manifest = profile_dir.AppendASCII(kProfileIdFile);
  const std::string binding = ProfileBinding(profile_dir);
  if (base::PathExists(manifest)) {
    std::string contents;
    std::string token;
    bool needs_rebind = false;
    if (!base::ReadFileToString(manifest, &contents) ||
        !ParseProfileId(contents, binding, &token, &needs_rebind)) {
      return false;  // corrupt/partial markers are never replaced implicitly
    }
    if (!needs_rebind) {
      *out = std::move(token);
      return true;
    }
    // A v1 marker, renamed profile, or copied profile directory needs a fresh
    // physical identity. RestoreFromDisk's generic scoped loader re-keys any
    // snapshots from the old token to this new one transactionally.
    token = base::UnguessableToken::Create().ToString();
    if (!ReplaceProfileId(profile_dir, ProfileIdContents(token, binding)))
      return false;
    *out = std::move(token);
    return true;
  }

  const std::string token = base::UnguessableToken::Create().ToString();
  const std::string contents = ProfileIdContents(token, binding);
  base::File file(manifest, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  if (!file.IsValid())
    return base::PathExists(manifest) && ReadProfileId(profile_dir, out);
  const bool ok = file.WriteAtCurrentPosAndCheck(base::as_byte_span(contents)) &&
                  file.Flush();
  file.Close();
  if (!ok) {
    base::DeleteFile(manifest);
    return false;
  }
  *out = token;
  return true;
}

bool BackupMigratedSnapshot(const base::FilePath& path) {
  if (!base::PathExists(path))
    return false;
  const std::string suffix =
      ".pre-scope-migration." + base::UnguessableToken::Create().ToString();
  const base::FilePath backup = base::FilePath::FromUTF8Unsafe(
      path.AsUTF8Unsafe() + suffix);
  if (base::CopyFile(path, backup))
    return true;
  base::DeleteFile(backup);
  return false;
}

// A persistent profile name is one directory component (<persist_path>/<name>),
// so it must be portable and unambiguous.
bool IsPortableProfileName(const std::string& name) {
  if (name.empty() || name == "." || name == "..")
    return false;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|' ||
        static_cast<unsigned char>(c) < 0x20)  // NUL + other control chars
      return false;
  }
  if (name.back() == '.' || name.back() == ' ')
    return false;
  const std::string stem = base::ToUpperASCII(name.substr(0, name.find('.')));
  static const char* const kReserved[] = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
      "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
      "LPT8", "LPT9"};
  for (const char* r : kReserved) {
    if (stem == r)
      return false;
  }
  return true;
}
}  // namespace

MbSession::MbSession(std::string name,
                     std::string persist_dir,
                     std::string id)
    : name_(std::move(name)),
      persist_dir_(std::move(persist_dir)),
      id_(std::move(id)) {}

// static
MbSession* MbSession::Default() {
  static MbSession* def = [] {
    auto* s = new MbSession("default", "", "e:default");
    s->is_default_ = true;
    return s;
  }();
  return def;
}

// static
MbSession* MbSession::Create(const std::string& name,
                             const std::string& persist_path,
                             MbSessionCreateResult* out_result) {
  if (out_result)
    *out_result = MbSessionCreateResult::kError;

  std::string dir;
  std::string id;
  if (persist_path.empty()) {
    // Ephemeral: every Create() is a DISTINCT profile, even if two callers pass the same
    // name — so give it a process-unique storage id. The singleton Default() keeps its
    // stable "e:default" id (it is constructed directly, not through Create).
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    id = "e:" + name + ":" + std::to_string(n);
  } else {
    if (!IsPortableProfileName(name)) {
      if (out_result)
        *out_result = MbSessionCreateResult::kInvalidName;
      return nullptr;
    }
    // Resolve the root, then create and resolve the FULL profile directory. The
    // second resolution is important on case-insensitive / Unicode-normalizing
    // filesystems, where two distinct name spellings can address one directory.
    // The durable identity is the marker inside that directory, not a spelling
    // of its path, so symlink/case/normalization aliases cannot split storage.
    base::FilePath root = base::FilePath::FromUTF8Unsafe(persist_path);
    if (root.empty() || !base::CreateDirectory(root))
      return nullptr;
    const base::FilePath canonical_root = base::MakeAbsoluteFilePath(root);
    if (canonical_root.empty())
      return nullptr;
    const base::FilePath requested_profile =
        canonical_root.Append(base::FilePath::FromUTF8Unsafe(name));
    if (requested_profile.empty() || !base::CreateDirectory(requested_profile))
      return nullptr;
    // Normalize the EXISTING full directory through the filesystem. Unlike
    // MakeAbsoluteFilePath on Windows, this resolves junctions/symlinks and
    // canonical casing (GetFinalPathNameByHandle), so aliases share one marker
    // binding on every platform.
    base::FilePath profile_dir;
    if (!base::NormalizeFilePath(requested_profile, &profile_dir) ||
        profile_dir.empty()) {
      return nullptr;
    }
    std::string profile_id;
    if (!LoadOrCreateProfileId(profile_dir, &profile_id))
      return nullptr;
    dir = profile_dir.AsUTF8Unsafe();
    id = "p:" + profile_id;
  }

  auto* session = new MbSession(name, dir, id);
  if (!dir.empty()) {
    RegisterProfile(id);
    session->registered_profile_state_ = true;
  }
  session->RestoreFromDisk();  // no-op for ephemeral / first-run profiles
  if (out_result)
    *out_result = MbSessionCreateResult::kOk;
  return session;
}

bool MbSession::FlushToDisk() {
  if (!persistent())
    return true;  // nothing to persist for an ephemeral profile -> success
  const base::FilePath dir = base::FilePath::FromUTF8Unsafe(persist_dir_);
  std::string profile_id;
  if (!base::CreateDirectory(dir) || !ReadProfileId(dir, &profile_id) ||
      id_ != "p:" + profile_id) {
    return false;  // never write snapshots without their matching identity
  }
  ProfileState* state = FindProfile(id_);
  if (!state)
    return false;

  // Best-effort per store, but never overwrite a snapshot that failed parsing
  // or whose legacy scope was ambiguous. Before a migrated snapshot is first
  // rewritten, retain its original bytes in a sibling backup. If the save then
  // fails, later retries keep that same known-good backup instead of backing up
  // a partially written replacement.
  bool ok = MbSaveCookies(dir.AppendASCII("cookies.dat").AsUTF8Unsafe(), id_);

  const base::FilePath idb_path = dir.AppendASCII("idb.dat");
  if (!state->idb_flush_safe) {
    ok = false;
  } else {
    if (state->idb_needs_backup && !state->idb_backup_complete)
      state->idb_backup_complete = BackupMigratedSnapshot(idb_path);
    if (state->idb_needs_backup && !state->idb_backup_complete) {
      ok = false;
    } else {
      const bool saved = MbSaveIndexedDBScoped(idb_path.AsUTF8Unsafe(),
                                               scope_prefix());
      ok &= saved;
      if (saved) {
        state->idb_needs_backup = false;
        state->idb_backup_complete = false;
      }
    }
  }

  const base::FilePath opfs_path = dir.AppendASCII("opfs.dat");
  if (!state->opfs_flush_safe) {
    ok = false;
  } else {
    if (state->opfs_needs_backup && !state->opfs_backup_complete)
      state->opfs_backup_complete = BackupMigratedSnapshot(opfs_path);
    if (state->opfs_needs_backup && !state->opfs_backup_complete) {
      ok = false;
    } else {
      const bool saved =
          MbSaveOPFSScoped(opfs_path.AsUTF8Unsafe(), scope_prefix());
      ok &= saved;
      if (saved) {
        state->opfs_needs_backup = false;
        state->opfs_backup_complete = false;
      }
    }
  }
  return ok;
}

void MbSession::RestoreFromDisk() {
  if (!persistent())
    return;
  ProfileState* state = FindProfile(id_);
  if (!state || state->restored)
    return;
  state->restored = true;

  const base::FilePath dir = base::FilePath::FromUTF8Unsafe(persist_dir_);
  // Best-effort per file: a first run has none of them.
  MbLoadCookies(dir.AppendASCII("cookies.dat").AsUTF8Unsafe(), id_);
  const base::FilePath idb_path = dir.AppendASCII("idb.dat");
  if (base::PathExists(idb_path)) {
    bool rekeyed = false;
    state->idb_flush_safe = MbLoadIndexedDBMergeScoped(
        idb_path.AsUTF8Unsafe(), scope_prefix(), &rekeyed);
    state->idb_needs_backup = state->idb_flush_safe && rekeyed;
  }
  const base::FilePath opfs_path = dir.AppendASCII("opfs.dat");
  if (base::PathExists(opfs_path)) {
    bool rekeyed = false;
    state->opfs_flush_safe =
        MbLoadOPFSScoped(opfs_path.AsUTF8Unsafe(), scope_prefix(), &rekeyed);
    state->opfs_needs_backup = state->opfs_flush_safe && rekeyed;
  }
}

void MbSession::ClearStorage() {
  MbClearCookieJar(id_);
  MbClearPageCookieStoreForSession(id_);
  MbClearDomStorageForSession(id_);
  MbClearIndexedDBScoped(scope_prefix());
  MbClearOPFSScoped(scope_prefix());
  // Explicit clearing authorizes replacing an unreadable legacy snapshot with
  // this intentionally empty state.
  if (ProfileState* state = FindProfile(id_)) {
    state->idb_flush_safe = true;
    state->opfs_flush_safe = true;
    state->idb_needs_backup = false;
    state->opfs_needs_backup = false;
    state->idb_backup_complete = false;
    state->opfs_backup_complete = false;
  }
}

void MbSession::Release() {
  if (refs_ > 0)
    --refs_;
  MaybeDelete();
}

void MbSession::Detach() {
  detached_ = true;
  MaybeDelete();
}

void MbSession::MaybeDelete() {
  if (is_default_ || !detached_ || refs_ != 0)
    return;
  FlushToDisk();  // final durability barrier for persistent profiles
  delete this;
}

MbSession::~MbSession() {
  if (registered_profile_state_)
    UnregisterProfile(id_);
  // Free the C-ABI handle now that the impl is gone — every view and view-config
  // has released it, so no mbViewGetSession/config can still be holding it. The
  // deleter runs in the C-ABI translation unit where mbSession is complete.
  if (host_handle_ && host_handle_deleter_)
    host_handle_deleter_(host_handle_);
}

}  // namespace mb
