#include "miniblink_host/frame/mb_broadcast_channel.h"

#include <algorithm>
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
#include "mojo/public/cpp/system/message_pipe.h"
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
      const blink::String& name,
      mojo::PendingAssociatedRemote<BroadcastChannelClient> client,
      mojo::PendingAssociatedReceiver<BroadcastChannelClient> connection)
      : name_(name.Utf8()),
        client_(std::move(client)),
        receiver_(this, std::move(connection)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&MbBroadcastChannel::OnDisconnect, base::Unretained(this)));
    BcRegistry()[name_].push_back(this);
  }

  // BroadcastChannelClient: a message posted by this channel's page. Fan it out to every
  // OTHER channel of the same name (the spec excludes the sender).
  void OnMessage(blink::BlinkCloneableMessage message) override {
    auto it = BcRegistry().find(name_);
    if (it == BcRegistry().end())
      return;
    for (MbBroadcastChannel* ch : it->second) {
      if (ch != this)
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

  std::string name_;
  mojo::AssociatedRemote<BroadcastChannelClient> client_;
  mojo::AssociatedReceiver<BroadcastChannelClient> receiver_;
};

class MbBroadcastChannelProvider : public BroadcastChannelProvider {
 public:
  void ConnectToChannel(
      const blink::String& name,
      mojo::PendingAssociatedRemote<BroadcastChannelClient> client,
      mojo::PendingAssociatedReceiver<BroadcastChannelClient> connection)
      override {
    new MbBroadcastChannel(name, std::move(client), std::move(connection));
  }
};

}  // namespace

void BindBroadcastChannelProvider(mojo::ScopedInterfaceEndpointHandle handle) {
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<MbBroadcastChannelProvider>(),
      mojo::PendingAssociatedReceiver<BroadcastChannelProvider>(
          std::move(handle)));
}

}  // namespace mb
