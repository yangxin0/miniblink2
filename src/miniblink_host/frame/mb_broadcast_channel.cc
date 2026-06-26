#include "miniblink_host/frame/mb_broadcast_channel.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "third_party/blink/public/mojom/broadcastchannel/broadcast_channel.mojom-blink.h"
#include "third_party/blink/renderer/core/messaging/blink_cloneable_message.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

using blink::mojom::blink::BroadcastChannelClient;
using blink::mojom::blink::BroadcastChannelProvider;

class MbBroadcastChannel;

// Process-wide registry: channel name -> all open channels with that name. The provider is
// bound on the service thread (shared with the blob nav-associated provider), so all access
// is single-threaded there; no lock needed. (Origin partitioning is omitted; the host is
// effectively single page-origin for messaging purposes.) std::string key (WTF::String has
// no std::less).
std::map<std::string, std::vector<MbBroadcastChannel*>>& BcRegistry() {
  static auto* m = new std::map<std::string, std::vector<MbBroadcastChannel*>>();
  return *m;
}

// BlinkCloneableMessage is move-only (it can carry a move-only transfer token), but for the
// no-transfer messages BroadcastChannel sends, every field is a shareable value or refcount
// — so a field-wise copy is a safe shallow clone for fan-out to multiple receivers.
blink::BlinkCloneableMessage CloneMessage(const blink::BlinkCloneableMessage& m) {
  blink::BlinkCloneableMessage c;
  c.message = m.message;  // SerializedScriptValue is immutable + refcounted
  c.sender_origin = m.sender_origin;
  c.sender_stack_trace_id = m.sender_stack_trace_id;
  c.sender_agent_cluster_id = m.sender_agent_cluster_id;
  c.locked_to_sender_agent_cluster = m.locked_to_sender_agent_cluster;
  c.trace_id = m.trace_id;
  return c;
}

// One open BroadcastChannel: receives the page's posts (as the `connection` receiver) and
// holds the `client` remote to deliver inbound messages to that page. Self-managed: removed
// from the registry and deleted when its pipe disconnects (channel closed / GC'd).
class MbBroadcastChannel : public BroadcastChannelClient {
 public:
  MbBroadcastChannel(
      uint64_t frame_key,
      const blink::String& name,
      mojo::PendingAssociatedRemote<BroadcastChannelClient> client,
      mojo::PendingAssociatedReceiver<BroadcastChannelClient> connection)
      : frame_key_(frame_key),
        name_(name.Utf8()),
        client_(std::move(client)),
        receiver_(this, std::move(connection)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&MbBroadcastChannel::OnDisconnect, base::Unretained(this)));
    BcRegistry()[name_].push_back(this);
  }

  // BroadcastChannelClient: a message posted by this channel's page. Fan it out to every
  // OTHER same-name channel of the SAME ORIGIN (BroadcastChannel is same-origin per spec).
  // Origins come from the frame_key->origin map; a channel whose origin is unknown (e.g. a
  // worker bound without a frame_key) acts as a wildcard so existing same-origin window<->
  // worker communication is preserved — we withhold ONLY when both origins are known and
  // differ. That isolates the common cross-origin window<->window case without regressions.
  void OnMessage(blink::BlinkCloneableMessage message) override {
    auto it = BcRegistry().find(name_);
    if (it == BcRegistry().end())
      return;
    // A CONCRETE origin is a known, non-opaque one. "" = unknown (no frame_key);
    // "null" = an opaque origin (a data:/blob: worker's). Both act as WILDCARDS so
    // window<->worker bridging is preserved (a data: worker is opaque-by-URL but
    // same-origin as its creator) — we withhold only between two concrete, DIFFERENT
    // origins (e.g. two same-named channels in different http(s) origins).
    auto concrete = [](const std::string& o) {
      return !o.empty() && o != "null";
    };
    const std::string sender_origin = MbGetFrameOrigin(frame_key_);
    for (MbBroadcastChannel* ch : it->second) {
      if (ch == this)
        continue;
      const std::string recv_origin = MbGetFrameOrigin(ch->frame_key_);
      if (concrete(sender_origin) && concrete(recv_origin) &&
          sender_origin != recv_origin) {
        continue;  // both concrete and cross-origin -> isolate
      }
      ch->client_->OnMessage(CloneMessage(message));
    }
  }

 private:
  void OnDisconnect() {
    // Don't delete the receiver inside its own disconnect handler — post it.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&MbBroadcastChannel::Destroy, base::Unretained(this)));
  }
  void Destroy() {
    auto it = BcRegistry().find(name_);
    if (it != BcRegistry().end()) {
      auto& v = it->second;
      v.erase(std::remove(v.begin(), v.end(), this), v.end());
      if (v.empty())
        BcRegistry().erase(it);
    }
    delete this;
  }

  const uint64_t frame_key_;
  std::string name_;
  mojo::AssociatedRemote<BroadcastChannelClient> client_;
  mojo::AssociatedReceiver<BroadcastChannelClient> receiver_;
};

class MbBroadcastChannelProvider : public BroadcastChannelProvider {
 public:
  explicit MbBroadcastChannelProvider(uint64_t frame_key)
      : frame_key_(frame_key) {}

  void ConnectToChannel(
      const blink::String& name,
      mojo::PendingAssociatedRemote<BroadcastChannelClient> client,
      mojo::PendingAssociatedReceiver<BroadcastChannelClient> connection)
      override {
    new MbBroadcastChannel(frame_key_, name, std::move(client),
                           std::move(connection));
  }

 private:
  const uint64_t frame_key_;
};

}  // namespace

void BindBroadcastChannelProvider(mojo::ScopedInterfaceEndpointHandle handle,
                                  uint64_t frame_key) {
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbBroadcastChannelProvider>(frame_key),
      mojo::PendingAssociatedReceiver<BroadcastChannelProvider>(
          std::move(handle)));
}

void BindBroadcastChannelProviderPipe(
    mojo::PendingReceiver<BroadcastChannelProvider> receiver,
    uint64_t frame_key) {
  // Worker path (via the frame interface broker, which now carries the worker's
  // synthetic frame_key -> its origin). For an http(s) worker that scopes the
  // channel by the worker's real origin; a data:/blob: worker is opaque ("null")
  // -> wildcard (so same-origin window<->worker still bridges).
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<MbBroadcastChannelProvider>(frame_key),
      std::move(receiver));
}

}  // namespace mb
