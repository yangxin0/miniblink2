#include "miniblink_host/frame/mb_websocket.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/websocket.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace mb {
namespace {

using network::mojom::blink::WebSocket;
using network::mojom::blink::WebSocketClient;
using network::mojom::blink::WebSocketHandshakeClient;
using network::mojom::blink::WebSocketMessageType;

// One WebSocket: implements the network WebSocket the page drives, and loops messages back to
// the page through its WebSocketClient. Self-deletes when the page drops the socket.
class MbWebSocket : public WebSocket {
 public:
  MbWebSocket(mojo::PendingReceiver<WebSocket> receiver,
              mojo::Remote<WebSocketClient> client,
              mojo::ScopedDataPipeProducerHandle readable_producer,
              mojo::ScopedDataPipeConsumerHandle writable_consumer)
      : receiver_(this, std::move(receiver)),
        client_(std::move(client)),
        readable_producer_(std::move(readable_producer)),
        writable_consumer_(std::move(writable_consumer)),
        out_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        in_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&MbWebSocket::OnGone, base::Unretained(this)));
    out_watcher_.Watch(
        writable_consumer_.get(), MOJO_HANDLE_SIGNAL_READABLE,
        base::BindRepeating(&MbWebSocket::OnOutgoingReadable,
                            base::Unretained(this)));
    in_watcher_.Watch(readable_producer_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                      base::BindRepeating(&MbWebSocket::DrainIncoming,
                                          base::Unretained(this)));
    out_watcher_.ArmOrNotify();
  }

  // network::mojom::blink::WebSocket:
  void SendMessage(WebSocketMessageType type, uint64_t data_length) override {
    pending_.push_back(Frame{type, data_length});
    out_watcher_.ArmOrNotify();
  }
  void StartReceiving() override {
    receiving_ = true;
    PumpIncoming();
  }
  void StartClosingHandshake(uint16_t code,
                             const blink::String& reason) override {
    if (client_)
      client_->OnDropChannel(/*was_clean=*/true, code, reason);
    // The page will drop the socket next; OnGone() cleans up.
  }

 private:
  struct Frame {
    WebSocketMessageType type;
    uint64_t length;
  };

  void OnGone() { delete this; }

  // Read everything available on the writable pipe (page -> us), then frame it into messages
  // (each SendMessage announced a length) and queue them to echo back.
  void OnOutgoingReadable(MojoResult) {
    for (;;) {
      char buf[8192];
      size_t read = 0;
      MojoResult rv = writable_consumer_->ReadData(
          MOJO_READ_DATA_FLAG_NONE,
          base::as_writable_bytes(base::span(buf)), read);
      if (rv == MOJO_RESULT_OK && read > 0) {
        in_buffer_.append(buf, read);
        continue;
      }
      if (rv == MOJO_RESULT_SHOULD_WAIT)
        out_watcher_.ArmOrNotify();
      break;  // SHOULD_WAIT / FAILED_PRECONDITION / empty
    }
    while (!pending_.empty() &&
           in_buffer_.size() >= pending_.front().length) {
      Frame f = pending_.front();
      pending_.erase(pending_.begin());
      echo_.push_back(
          Echo{f.type, in_buffer_.substr(0, static_cast<size_t>(f.length))});
      in_buffer_.erase(0, static_cast<size_t>(f.length));
    }
    PumpIncoming();
  }

  // Announce each queued echo as a data frame and buffer its bytes; then push to the pipe.
  void PumpIncoming() {
    if (!receiving_)
      return;
    while (!echo_.empty()) {
      Echo e = std::move(echo_.front());
      echo_.erase(echo_.begin());
      if (client_)
        client_->OnDataFrame(/*fin=*/true, e.type, e.bytes.size());
      write_buffer_ += e.bytes;
    }
    DrainIncoming(MOJO_RESULT_OK);
  }

  // Write pending echo bytes to the readable pipe (us -> page), honoring backpressure.
  void DrainIncoming(MojoResult) {
    while (write_pos_ < write_buffer_.size()) {
      size_t written = 0;
      base::span<const uint8_t> data = base::as_byte_span(write_buffer_)
                                           .subspan(write_pos_);
      MojoResult rv = readable_producer_->WriteData(
          data, MOJO_WRITE_DATA_FLAG_NONE, written);
      if (rv == MOJO_RESULT_OK && written > 0) {
        write_pos_ += written;
        continue;
      }
      if (rv == MOJO_RESULT_SHOULD_WAIT)
        in_watcher_.ArmOrNotify();
      return;
    }
    write_buffer_.clear();
    write_pos_ = 0;
  }

  struct Echo {
    WebSocketMessageType type;
    std::string bytes;
  };

  mojo::Receiver<WebSocket> receiver_;
  mojo::Remote<WebSocketClient> client_;
  mojo::ScopedDataPipeProducerHandle readable_producer_;  // us -> page
  mojo::ScopedDataPipeConsumerHandle writable_consumer_;  // page -> us
  mojo::SimpleWatcher out_watcher_;
  mojo::SimpleWatcher in_watcher_;
  bool receiving_ = false;
  std::vector<Frame> pending_;  // SendMessage announcements awaiting their bytes
  std::string in_buffer_;       // outgoing bytes read, not yet framed
  std::vector<Echo> echo_;      // framed messages awaiting delivery
  std::string write_buffer_;    // echo bytes awaiting the readable pipe
  size_t write_pos_ = 0;
};

class MbWebSocketConnector
    : public blink::mojom::blink::WebSocketConnector {
 public:
  void Connect(const blink::KURL& url,
               const blink::Vector<blink::String>& /*requested_protocols*/,
               const blink::String& /*user_agent*/,
               net::StorageAccessApiStatus /*storage_access_api_status*/,
               mojo::PendingRemote<WebSocketHandshakeClient> handshake_client,
               const std::optional<base::UnguessableToken>&
               /*throttling_profile_id*/) override {
    mojo::Remote<WebSocketHandshakeClient> client(std::move(handshake_client));

    // Two pipes: readable = us -> page (incoming), writable = page -> us (outgoing).
    mojo::ScopedDataPipeProducerHandle readable_producer, writable_producer;
    mojo::ScopedDataPipeConsumerHandle readable_consumer, writable_consumer;
    if (mojo::CreateDataPipe(nullptr, readable_producer, readable_consumer) !=
            MOJO_RESULT_OK ||
        mojo::CreateDataPipe(nullptr, writable_producer, writable_consumer) !=
            MOJO_RESULT_OK) {
      client->OnFailure("data pipe", 0, 0);
      return;
    }

    mojo::PendingRemote<WebSocket> socket_remote;
    auto socket_receiver = socket_remote.InitWithNewPipeAndPassReceiver();
    mojo::Remote<WebSocketClient> ws_client;
    auto ws_client_receiver = ws_client.BindNewPipeAndPassReceiver();

    // All non-nullable fields must be set or mojo serialization FATALs.
    auto response = network::mojom::blink::WebSocketHandshakeResponse::New();
    response->url = url;
    response->http_version = network::mojom::blink::HttpVersion::New(1, 1);
    response->status_code = 101;
    response->status_text = blink::String("Switching Protocols");
    response->remote_endpoint = net::IPEndPoint();
    response->headers_text = blink::String("");
    response->selected_protocol = blink::String("");
    response->extensions = blink::String("");

    client->OnConnectionEstablished(
        std::move(socket_remote), std::move(ws_client_receiver),
        std::move(response), std::move(readable_consumer),
        std::move(writable_producer));

    // Self-managed: deleted when the page drops the socket.
    new MbWebSocket(std::move(socket_receiver), std::move(ws_client),
                    std::move(readable_producer), std::move(writable_consumer));
  }
};

}  // namespace

void BindWebSocketConnector(
    mojo::PendingReceiver<blink::mojom::blink::WebSocketConnector> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebSocketConnector>(),
                              std::move(receiver));
}

}  // namespace mb
