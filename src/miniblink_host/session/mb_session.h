// A browsing profile (IMPROVEMENT.md item 12). Stage 1: identity + the storage
// partition prefix - every origin-keyed service scopes by id(), so two
// sessions never see each other's storage. Ephemeral unless created with a
// persist path (persistence itself lands in stage 3; the id/layout are
// already stable for it). Views borrow the pointer with AddRef/Release;
// Detach() is the owner-handle destructor - the object dies when it is
// detached AND the last view released it, so destroy order can't dangle.

#ifndef MINIBLINK_HOST_SESSION_MB_SESSION_H_
#define MINIBLINK_HOST_SESSION_MB_SESSION_H_

#include <string>

struct mbSession;  // the C-ABI handle (miniblink2.cc); one per MbSession

namespace mb {

enum class MbSessionCreateResult {
  kOk,
  kInvalidName,
  kError,
};

class MbSession {
 public:
  // The shared implicit profile for plain mbCreateView: ephemeral, never
  // deleted, zero-diff with the pre-session world.
  static MbSession* Default();
  static MbSession* Create(const std::string& name,
                           const std::string& persist_path,
                           MbSessionCreateResult* out_result = nullptr);
  // Compatibility path for the original mbCreateSession symbol. It preserves
  // that entry point's permissive profile-name handling; new callers should use
  // Create()/mbCreateSessionEx so malformed persistent names are rejected with
  // a structured status.
  static MbSession* CreateLegacy(const std::string& name,
                                 const std::string& persist_path);

  const std::string& id() const { return id_; }
  const std::string& name() const { return name_; }
  bool persistent() const { return !persist_dir_.empty(); }
  const std::string& persist_dir() const { return persist_dir_; }

  void AddRef() { ++refs_; }
  void Release();
  void Detach();

  // Stage 3 (persistence): persistent sessions restore at Create and flush at
  // FlushToDisk / final teardown; ephemeral sessions no-op. ClearStorage wipes
  // this profile's cookies, DOM-storage backend, IndexedDB, and OPFS. Live
  // localStorage caches are invalidated; Blink does not permit backend-driven
  // sessionStorage invalidation, so a live document may retain that cache.
  // Returns true if everything was persisted (or nothing needed to be, for an
  // ephemeral profile); false if the profile dir or any store could not be
  // written — the C API surfaces this via mbSessionFlush's return.
  bool FlushToDisk();
  void RestoreFromDisk();
  void ClearStorage();
  // The prefix every partition scope of this session starts with.
  std::string scope_prefix() const { return id_ + "\x1f"; }

  void set_host_handle(mbSession* h) { host_handle_ = h; }
  mbSession* host_handle() const { return host_handle_; }
  // The C-ABI layer registers a deleter for its opaque mbSession handle so the
  // handle is freed HERE — when the impl is actually destroyed (every view and
  // view-config has released it) — instead of eagerly in mbDestroySession. That
  // is what lets mbViewGetSession keep returning a live handle while views remain
  // open; freeing the handle at Detach time (with the impl still alive) is the
  // use-after-free this avoids. Freed via the deleter in ~MbSession because the
  // mbSession type is only complete in the C-ABI translation unit.
  void set_host_handle_deleter(void (*fn)(mbSession*)) {
    host_handle_deleter_ = fn;
  }

 private:
  static MbSession* CreateImpl(const std::string& name,
                               const std::string& persist_path,
                               bool validate_name,
                               MbSessionCreateResult* out_result);
  MbSession(std::string name, std::string persist_dir, std::string id);
  ~MbSession();
  void MaybeDelete();

  std::string name_;
  std::string persist_dir_;  // "<persist_path>/<name>" or "" (ephemeral)
  std::string id_;           // storage partition prefix; stable per profile
  int refs_ = 0;             // live views bound to this session
  bool detached_ = false;    // owner handle destroyed
  bool is_default_ = false;
  bool registered_profile_state_ = false;
  mbSession* host_handle_ = nullptr;  // freed by host_handle_deleter_ in ~MbSession
  void (*host_handle_deleter_)(mbSession*) = nullptr;  // set by the C-ABI layer
};

}  // namespace mb

#endif  // MINIBLINK_HOST_SESSION_MB_SESSION_H_
