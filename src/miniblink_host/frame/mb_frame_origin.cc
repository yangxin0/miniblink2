// mb_frame_origin.cc — see header.
#include "miniblink_host/frame/mb_frame_origin.h"

#include <map>

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace mb {
namespace {

struct OriginMap {
  base::Lock lock;
  std::map<uint64_t, std::string> by_frame;
};

OriginMap& Map() {
  static base::NoDestructor<OriginMap> m;
  return *m;
}

}  // namespace

void MbSetFrameOrigin(uint64_t frame_key, const std::string& origin) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  m.by_frame[frame_key] = origin;
}

void MbClearFrameOrigin(uint64_t frame_key) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  m.by_frame.erase(frame_key);
}

std::string MbGetFrameOrigin(uint64_t frame_key) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  auto it = m.by_frame.find(frame_key);
  return it == m.by_frame.end() ? std::string() : it->second;
}

}  // namespace mb
