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

class MbSession {
 public:
  // The shared implicit profile for plain mbCreateView: ephemeral, never
  // deleted, zero-diff with the pre-session world.
  static MbSession* Default();
  static MbSession* Create(const std::string& name,
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
  // this profile's cookies + IndexedDB + OPFS (live documents' DOM storage is
  // blink-internal and not reachable service-side - documented gap).
  void FlushToDisk();
  void RestoreFromDisk();
  void ClearStorage();
  // The prefix every partition scope of this session starts with.
  std::string scope_prefix() const { return id_ + "\x1f"; }

  void set_host_handle(mbSession* h) { host_handle_ = h; }
  mbSession* host_handle() const { return host_handle_; }

 private:
  MbSession(std::string name, std::string persist_dir);
  ~MbSession() = default;
  void MaybeDelete();

  std::string name_;
  std::string persist_dir_;  // "<persist_path>/<name>" or "" (ephemeral)
  std::string id_;           // storage partition prefix; stable per profile
  int refs_ = 0;             // live views bound to this session
  bool detached_ = false;    // owner handle destroyed
  bool is_default_ = false;
  mbSession* host_handle_ = nullptr;  // not owned
};

}  // namespace mb

#endif  // MINIBLINK_HOST_SESSION_MB_SESSION_H_
