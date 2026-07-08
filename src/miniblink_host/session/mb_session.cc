#include "miniblink_host/session/mb_session.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "miniblink_host/loader/mb_url_loader.h"

namespace mb {

MbSession::MbSession(std::string name, std::string persist_dir)
    : name_(std::move(name)), persist_dir_(std::move(persist_dir)) {
  // The id doubles as the storage partition prefix. A persistent profile's id
  // must be stable across runs (same name + path reopens the same storage);
  // an ephemeral one only needs process-lifetime uniqueness, which name
  // collisions could break - callers get what they name, like Ultralight.
  id_ = persist_dir_.empty() ? ("e:" + name_) : ("p:" + persist_dir_);
}

// static
MbSession* MbSession::Default() {
  static MbSession* def = [] {
    auto* s = new MbSession("default", "");
    s->is_default_ = true;
    return s;
  }();
  return def;
}

// static
MbSession* MbSession::Create(const std::string& name,
                             const std::string& persist_path) {
  std::string dir;
  if (!persist_path.empty()) {
    dir = persist_path;
    if (dir.back() != '/')
      dir += '/';
    dir += name;
  }
  auto* session = new MbSession(name, dir);
  session->RestoreFromDisk();  // no-op for ephemeral / first-run profiles
  return session;
}

void MbSession::FlushToDisk() {
  if (!persistent())
    return;
  const base::FilePath dir = base::FilePath::FromUTF8Unsafe(persist_dir_);
  base::CreateDirectory(dir);
  MbSaveCookies(dir.AppendASCII("cookies.dat").AsUTF8Unsafe(), id_);
  MbSaveIndexedDBScoped(dir.AppendASCII("idb.dat").AsUTF8Unsafe(), scope_prefix());
  MbSaveOPFSScoped(dir.AppendASCII("opfs.dat").AsUTF8Unsafe(), scope_prefix());
}

void MbSession::RestoreFromDisk() {
  if (!persistent())
    return;
  const base::FilePath dir = base::FilePath::FromUTF8Unsafe(persist_dir_);
  // Best-effort per file: a first run has none of them.
  MbLoadCookies(dir.AppendASCII("cookies.dat").AsUTF8Unsafe(), id_);
  MbLoadIndexedDBMerge(dir.AppendASCII("idb.dat").AsUTF8Unsafe());
  MbLoadOPFS(dir.AppendASCII("opfs.dat").AsUTF8Unsafe());  // MbLoadOPFS merges per scope
}

void MbSession::ClearStorage() {
  MbClearCookieJar(id_);
  MbClearIndexedDBScoped(scope_prefix());
  MbClearOPFSScoped(scope_prefix());
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

}  // namespace mb
