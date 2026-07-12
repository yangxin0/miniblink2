// mb_frame_origin.cc — see header.
#include "miniblink_host/frame/mb_frame_origin.h"

#include <atomic>
#include <map>

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace mb {
namespace {

struct OriginMap {
  base::Lock lock;
  std::map<uint64_t, std::string> by_frame;
  std::map<blink::LocalFrameToken, uint64_t> by_token;
  std::map<uint64_t, blink::LocalFrameToken> token_by_frame;
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
  auto token = m.token_by_frame.find(frame_key);
  if (token != m.token_by_frame.end()) {
    m.by_token.erase(token->second);
    m.token_by_frame.erase(token);
  }
}

void MbSetFrameToken(uint64_t frame_key,
                     const blink::LocalFrameToken& frame_token) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  auto old = m.token_by_frame.find(frame_key);
  if (old != m.token_by_frame.end())
    m.by_token.erase(old->second);
  m.by_token[frame_token] = frame_key;
  m.token_by_frame.insert_or_assign(frame_key, frame_token);
}

std::string MbGetFrameScopeForToken(
    const blink::LocalFrameToken& frame_token) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  auto token = m.by_token.find(frame_token);
  if (token == m.by_token.end())
    return std::string();
  auto scope = m.by_frame.find(token->second);
  return scope == m.by_frame.end() ? std::string() : scope->second;
}

std::string MbGetFrameOrigin(uint64_t frame_key) {
  OriginMap& m = Map();
  base::AutoLock guard(m.lock);
  auto it = m.by_frame.find(frame_key);
  return it == m.by_frame.end() ? std::string() : it->second;
}

uint64_t MbAllocWorkerFrameKey() {
  // High bit set -> disjoint from window keys (small ++counter values from
  // MbFrameClient), which never approach 2^63. Atomic: workers can be created
  // from different threads.
  static std::atomic<uint64_t> counter{0};
  return (uint64_t{1} << 63) | (++counter);
}

}  // namespace mb
