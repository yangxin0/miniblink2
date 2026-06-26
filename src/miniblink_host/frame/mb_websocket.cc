#include "miniblink_host/frame/mb_websocket.h"

#include <fcntl.h>
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "services/network/public/mojom/websocket.mojom-blink.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

#include "curl/curl.h"
#include "curl/websockets.h"

namespace mb {
namespace {

using network::mojom::blink::WebSocket;
using network::mojom::blink::WebSocketClient;
using network::mojom::blink::WebSocketHandshakeClient;
using network::mojom::blink::WebSocketMessageType;

// Reserved-TLD WS hosts (RFC 6761 ".test" can never be a real server) use the
// in-process LOOPBACK echo, so the WebSocket mojo data plane stays testable
// offline; every other host gets a REAL connection over libcurl's ws/wss.
bool IsLoopbackHost(const blink::KURL& url) {
  std::string host = url.Host().ToString().Utf8();
  const std::string kTest = ".test";
  return host.size() >= kTest.size() &&
         host.compare(host.size() - kTest.size(), kTest.size(), kTest) == 0;
}

network::mojom::blink::WebSocketHandshakeResponsePtr MakeHandshake101(
    const blink::KURL& url) {
  // All non-nullable fields must be set or mojo serialization FATALs.
  auto r = network::mojom::blink::WebSocketHandshakeResponse::New();
  r->url = url;
  r->http_version = network::mojom::blink::HttpVersion::New(1, 1);
  r->status_code = 101;
  r->status_text = blink::String("Switching Protocols");
  r->remote_endpoint = net::IPEndPoint();
  r->headers_text = blink::String("");
  r->selected_protocol = blink::String("");
  r->extensions = blink::String("");
  return r;
}

// ---- Real WebSocket transport over libcurl --------------------------------
// Owns a CURL* doing a real ws/wss connection. The blocking handshake + the
// send/recv loop run on a DETACHED worker thread (we never join from the service
// thread — teardown just flips `stop_`); the worker holds a shared_ptr to keep
// this alive until it exits. Results hop to the owning (service) thread through
// the callbacks, which the owner binds via base::BindPostTask + a WeakPtr so a
// dropped socket simply discards them. curl_ws_recv polls a non-blocking socket.
class CurlWsTransport : public std::enable_shared_from_this<CurlWsTransport> {
 public:
  using ConnectedCb = base::OnceCallback<void(bool ok)>;
  using MessageCb = base::RepeatingCallback<void(bool is_text, std::string)>;
  using ClosedCb = base::OnceCallback<void()>;

  CurlWsTransport(std::string url,
                  ConnectedCb connected,
                  MessageCb message,
                  ClosedCb closed)
      : url_(std::move(url)),
        connected_(std::move(connected)),
        message_(std::move(message)),
        closed_(std::move(closed)) {}

  void Start() {
    auto self = shared_from_this();  // keep alive for the worker's lifetime
    std::thread([self] { self->ThreadMain(); }).detach();
  }

  void Send(bool is_text, std::string bytes) {  // service thread -> queue
    std::lock_guard<std::mutex> g(mu_);
    outgoing_.push_back({is_text, std::move(bytes)});
  }
  void Stop() { stop_.store(true); }

 private:
  void ThreadMain() {
    CURL* c = curl_easy_init();
    if (!c) {
      std::move(connected_).Run(false);
      return;
    }
    curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);  // 2 = WebSocket
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    CURLcode rc = curl_easy_perform(c);  // blocking ws/wss handshake
    if (rc != CURLE_OK) {
      curl_easy_cleanup(c);
      std::move(connected_).Run(false);
      return;
    }
    // Non-blocking socket so curl_ws_recv returns CURLE_AGAIN (vs blocking),
    // letting the loop observe `stop_` promptly.
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK &&
        sock != CURL_SOCKET_BAD) {
      int fl = ::fcntl(sock, F_GETFL, 0);
      if (fl != -1)
        ::fcntl(sock, F_SETFL, fl | O_NONBLOCK);
    }
    std::move(connected_).Run(true);  // -> establish + onopen on service thread

    std::string frag;       // accumulate a partial / multi-frame message
    bool frag_text = true;  // type comes from the first fragment
    while (!stop_.load()) {
      for (;;) {  // drain queued outgoing messages
        std::pair<bool, std::string> msg;
        {
          std::lock_guard<std::mutex> g(mu_);
          if (outgoing_.empty())
            break;
          msg = std::move(outgoing_.front());
          outgoing_.pop_front();
        }
        size_t sent = 0;
        curl_ws_send(c, msg.second.data(), msg.second.size(), &sent, 0,
                     msg.first ? CURLWS_TEXT : CURLWS_BINARY);
      }
      char buf[16384];
      size_t got = 0;
      const struct curl_ws_frame* meta = nullptr;
      rc = curl_ws_recv(c, buf, sizeof(buf), &got, &meta);
      if (rc == CURLE_OK && meta) {
        if (meta->flags & CURLWS_CLOSE)
          break;  // server-initiated close
        if (meta->flags & (CURLWS_TEXT | CURLWS_BINARY))
          frag_text = (meta->flags & CURLWS_TEXT) != 0;
        if (got)
          frag.append(buf, got);
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
          message_.Run(frag_text, std::move(frag));
          frag.clear();
        }
        continue;  // drain buffered data before sleeping
      }
      if (rc == CURLE_AGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      break;  // connection error / closed
    }
    size_t sent = 0;
    curl_ws_send(c, "", 0, &sent, 0, CURLWS_CLOSE);  // best-effort close
    curl_easy_cleanup(c);
    std::move(closed_).Run();  // -> onclose on the service thread (if alive)
  }

  std::string url_;
  ConnectedCb connected_;
  MessageCb message_;
  ClosedCb closed_;
  std::mutex mu_;
  std::deque<std::pair<bool, std::string>> outgoing_;  // guarded by mu_
  std::atomic<bool> stop_{false};
};

// One WebSocket the page drives. LOOPBACK echoes the page's messages in-process
// (offline-testable); REAL bridges the mojo data plane to a CurlWsTransport. A
// REAL socket starts in a CONNECTING state holding the deferred OnConnection-
// Established payload, which it consumes when the transport reports success (so
// onopen reflects a real connection) or discards (OnFailure) on failure.
// Self-deletes when the page drops the socket or a real connect fails.
class MbWebSocket : public WebSocket {
 public:
  enum Mode { kLoopback, kReal };

  // Deferred-establish payload (REAL only): the page's pipe ends + remotes, held
  // until the handshake lands.
  struct Pending {
    mojo::Remote<WebSocketHandshakeClient> handshake_client;
    blink::KURL url;
    mojo::PendingRemote<WebSocket> socket_remote;
    mojo::PendingReceiver<WebSocketClient> ws_client_receiver;
    mojo::ScopedDataPipeConsumerHandle readable_consumer;  // page reads incoming
    mojo::ScopedDataPipeProducerHandle writable_producer;  // page writes outgoing
  };

  MbWebSocket(Mode mode,
              mojo::PendingReceiver<WebSocket> receiver,
              mojo::Remote<WebSocketClient> client,
              mojo::ScopedDataPipeProducerHandle readable_producer,
              mojo::ScopedDataPipeConsumerHandle writable_consumer,
              std::unique_ptr<Pending> pending)
      : mode_(mode),
        receiver_(this, std::move(receiver)),
        client_(std::move(client)),
        readable_producer_(std::move(readable_producer)),
        writable_consumer_(std::move(writable_consumer)),
        out_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        in_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        pending_establish_(std::move(pending)) {
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

  ~MbWebSocket() override {
    if (transport_)
      transport_->Stop();  // detached worker observes stop_ and exits
  }

  void SetTransport(std::shared_ptr<CurlWsTransport> t) {
    transport_ = std::move(t);
  }
  base::WeakPtr<MbWebSocket> GetWeakPtr() { return weak_.GetWeakPtr(); }

  // CurlWsTransport callbacks (service thread, WeakPtr-guarded):
  void OnTransportConnected(bool ok) {
    std::unique_ptr<Pending> p = std::move(pending_establish_);
    if (!p)
      return;
    if (!ok) {
      p->handshake_client->OnFailure("websocket connect failed",
                                     net::ERR_FAILED, -1);
      delete this;  // never established; nothing else references us
      return;
    }
    // Hand the page its endpoints — onopen fires now, on a real connection.
    p->handshake_client->OnConnectionEstablished(
        std::move(p->socket_remote), std::move(p->ws_client_receiver),
        MakeHandshake101(p->url), std::move(p->readable_consumer),
        std::move(p->writable_producer));
  }
  void OnTransportMessage(bool is_text, std::string bytes) {
    DeliverToPage(is_text ? WebSocketMessageType::TEXT
                          : WebSocketMessageType::BINARY,
                  bytes);
  }
  void OnTransportClosed() {
    if (client_)
      client_->OnDropChannel(/*was_clean=*/true, 1000, blink::String(""));
  }

  // network::mojom::blink::WebSocket:
  void SendMessage(WebSocketMessageType type, uint64_t data_length) override {
    pending_.push_back(Frame{type, data_length});
    out_watcher_.ArmOrNotify();
  }
  void StartReceiving() override {
    receiving_ = true;
    PumpQueued();
  }
  void StartClosingHandshake(uint16_t code,
                             const blink::String& reason) override {
    if (transport_)
      transport_->Stop();
    if (client_)
      client_->OnDropChannel(/*was_clean=*/true, code, reason);
  }

 private:
  struct Frame {
    WebSocketMessageType type;
    uint64_t length;
  };
  struct Echo {
    WebSocketMessageType type;
    std::string bytes;
  };

  void OnGone() { delete this; }

  // Read the writable pipe (page -> us), frame per SendMessage announcements,
  // then dispatch each complete message: LOOPBACK echoes; REAL sends via curl.
  void OnOutgoingReadable(MojoResult) {
    for (;;) {
      char buf[8192];
      size_t read = 0;
      MojoResult rv = writable_consumer_->ReadData(
          MOJO_READ_DATA_FLAG_NONE, base::as_writable_bytes(base::span(buf)),
          read);
      if (rv == MOJO_RESULT_OK && read > 0) {
        in_buffer_.append(buf, read);
        continue;
      }
      if (rv == MOJO_RESULT_SHOULD_WAIT)
        out_watcher_.ArmOrNotify();
      break;
    }
    while (!pending_.empty() && in_buffer_.size() >= pending_.front().length) {
      Frame f = pending_.front();
      pending_.erase(pending_.begin());
      std::string msg = in_buffer_.substr(0, static_cast<size_t>(f.length));
      in_buffer_.erase(0, static_cast<size_t>(f.length));
      if (mode_ == kReal) {
        if (transport_)
          transport_->Send(f.type == WebSocketMessageType::TEXT,
                           std::move(msg));
      } else {
        queued_.push_back(Echo{f.type, std::move(msg)});
      }
    }
    PumpQueued();
  }

  void PumpQueued() {
    if (!receiving_)
      return;
    while (!queued_.empty()) {
      Echo e = std::move(queued_.front());
      queued_.erase(queued_.begin());
      DeliverToPage(e.type, e.bytes);
    }
  }

  // Deliver one complete message to the page: announce the frame, push bytes.
  void DeliverToPage(WebSocketMessageType type, const std::string& bytes) {
    if (!receiving_) {  // server/echo data before the page armed receiving
      queued_.push_back(Echo{type, bytes});
      return;
    }
    if (client_)
      client_->OnDataFrame(/*fin=*/true, type, bytes.size());
    write_buffer_ += bytes;
    DrainIncoming(MOJO_RESULT_OK);
  }

  void DrainIncoming(MojoResult) {
    while (write_pos_ < write_buffer_.size()) {
      size_t written = 0;
      base::span<const uint8_t> data =
          base::as_byte_span(write_buffer_).subspan(write_pos_);
      MojoResult rv =
          readable_producer_->WriteData(data, MOJO_WRITE_DATA_FLAG_NONE,
                                        written);
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

  Mode mode_;
  mojo::Receiver<WebSocket> receiver_;
  mojo::Remote<WebSocketClient> client_;
  mojo::ScopedDataPipeProducerHandle readable_producer_;  // us -> page
  mojo::ScopedDataPipeConsumerHandle writable_consumer_;  // page -> us
  mojo::SimpleWatcher out_watcher_;
  mojo::SimpleWatcher in_watcher_;
  bool receiving_ = false;
  std::vector<Frame> pending_;  // SendMessage announcements awaiting bytes
  std::string in_buffer_;       // outgoing bytes read, not yet framed
  std::vector<Echo> queued_;    // messages awaiting delivery to the page
  std::string write_buffer_;    // incoming bytes awaiting the readable pipe
  size_t write_pos_ = 0;
  std::shared_ptr<CurlWsTransport> transport_;       // kReal only
  std::unique_ptr<Pending> pending_establish_;       // kReal, until connected
  base::WeakPtrFactory<MbWebSocket> weak_{this};
};

class MbWebSocketConnector : public blink::mojom::blink::WebSocketConnector {
 public:
  void Connect(const blink::KURL& url,
               const blink::Vector<blink::String>& /*requested_protocols*/,
               const blink::String& /*user_agent*/,
               net::StorageAccessApiStatus /*storage_access_api_status*/,
               mojo::PendingRemote<WebSocketHandshakeClient> handshake_client,
               const std::optional<base::UnguessableToken>&
               /*throttling_profile_id*/) override {
    mojo::Remote<WebSocketHandshakeClient> client(std::move(handshake_client));

    // readable = us -> page (incoming), writable = page -> us (outgoing).
    mojo::ScopedDataPipeProducerHandle readable_producer, writable_producer;
    mojo::ScopedDataPipeConsumerHandle readable_consumer, writable_consumer;
    if (mojo::CreateDataPipe(nullptr, readable_producer, readable_consumer) !=
            MOJO_RESULT_OK ||
        mojo::CreateDataPipe(nullptr, writable_producer, writable_consumer) !=
            MOJO_RESULT_OK) {
      client->OnFailure("data pipe", net::ERR_FAILED, -1);
      return;
    }

    mojo::PendingRemote<WebSocket> socket_remote;
    auto socket_receiver = socket_remote.InitWithNewPipeAndPassReceiver();
    mojo::Remote<WebSocketClient> ws_client;
    auto ws_client_receiver = ws_client.BindNewPipeAndPassReceiver();

    if (IsLoopbackHost(url)) {
      // Offline data-plane: establish immediately, echo in-process.
      client->OnConnectionEstablished(
          std::move(socket_remote), std::move(ws_client_receiver),
          MakeHandshake101(url), std::move(readable_consumer),
          std::move(writable_producer));
      new MbWebSocket(MbWebSocket::kLoopback, std::move(socket_receiver),
                      std::move(ws_client), std::move(readable_producer),
                      std::move(writable_consumer), /*pending=*/nullptr);
      return;
    }

    // REAL: build the socket NOW (connecting), defer OnConnectionEstablished
    // until the libcurl handshake succeeds. The socket holds the page's endpoints
    // in `pending`; its WeakPtr receives the transport's connected/message/closed
    // callbacks (so no early frame is lost). On connect failure it self-deletes.
    auto pending = std::make_unique<MbWebSocket::Pending>();
    pending->handshake_client = std::move(client);
    pending->url = url;
    pending->socket_remote = std::move(socket_remote);
    pending->ws_client_receiver = std::move(ws_client_receiver);
    pending->readable_consumer = std::move(readable_consumer);
    pending->writable_producer = std::move(writable_producer);

    auto* socket = new MbWebSocket(
        MbWebSocket::kReal, std::move(socket_receiver), std::move(ws_client),
        std::move(readable_producer), std::move(writable_consumer),
        std::move(pending));

    auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    base::WeakPtr<MbWebSocket> weak = socket->GetWeakPtr();
    auto transport = std::make_shared<CurlWsTransport>(
        url.GetString().Utf8(),
        base::BindPostTask(
            runner, base::BindOnce(&MbWebSocket::OnTransportConnected, weak)),
        base::BindPostTask(
            runner,
            base::BindRepeating(&MbWebSocket::OnTransportMessage, weak)),
        base::BindPostTask(
            runner, base::BindOnce(&MbWebSocket::OnTransportClosed, weak)));
    socket->SetTransport(transport);
    transport->Start();
  }
};

}  // namespace

void BindWebSocketConnector(
    mojo::PendingReceiver<blink::mojom::blink::WebSocketConnector> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbWebSocketConnector>(),
                              std::move(receiver));
}

}  // namespace mb
