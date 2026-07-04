#include "miniblink_host/session/mb_session.h"

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
  return new MbSession(name, dir);
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
  if (!is_default_ && detached_ && refs_ == 0)
    delete this;
}

}  // namespace mb
