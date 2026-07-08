#include "miniblink_host/frame/mb_websocket.h"

#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#else
#include <fcntl.h>
#endif
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
#include "base/strings/string_util.h"
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
    const blink::KURL& url,
    const std::string& selected_protocol) {
  // All non-nullable fields must be set or mojo serialization FATALs.
  auto r = network::mojom::blink::WebSocketHandshakeResponse::New();
  r->url = url;
  r->http_version = network::mojom::blink::HttpVersion::New(1, 1);
  r->status_code = 101;
  r->status_text = blink::String("Switching Protocols");
  r->remote_endpoint = net::IPEndPoint();
  r->headers_text = blink::String("");
  r->selected_protocol = blink::String::FromUtf8(selected_protocol);
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
  using ConnectedCb =
      base::OnceCallback<void(bool ok, std::string selected_protocol)>;
  using MessageCb = base::RepeatingCallback<void(bool is_text, std::string)>;
  // was_clean + RFC6455 close code + (UTF-8) reason.
  using ClosedCb =
      base::OnceCallback<void(bool was_clean, uint16_t code, std::string reason)>;

  // Largest reassembled incoming message we accept before failing with 1009.
  static constexpr size_t kMaxMessageBytes = 64 * 1024 * 1024;  // 64 MiB

  CurlWsTransport(std::string url,
                  std::string protocols,
                  ConnectedCb connected,
                  MessageCb message,
                  ClosedCb closed)
      : url_(std::move(url)),
        protocols_(std::move(protocols)),
        connected_(std::move(connected)),
        message_(std::move(message)),
        closed_(std::move(closed)) {}

  void Start() {
    auto self = shared_from_this();  // keep alive for the worker's lifetime
    std::thread([self] { self->ThreadMain(); }).detach();
  }

  // service thread -> queue. `flags` carries the curl frame opcode bits
  // (CURLWS_TEXT/CURLWS_BINARY[/CURLWS_CONT]).
  void Send(unsigned int flags, std::string bytes) {
    std::lock_guard<std::mutex> g(mu_);
    outgoing_.push_back({flags, std::move(bytes)});
  }
  // Page-initiated close: enqueue a CLOSE frame carrying the encoded code +
  // reason so the peer sees the real reason; the worker transmits it and then
  // exits, reporting OnDropChannel with these values.
  void Close(uint16_t code, std::string reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xff));
    payload.push_back(static_cast<char>(code & 0xff));
    payload += reason;
    std::lock_guard<std::mutex> g(mu_);
    page_close_code_ = code;
    page_close_reason_ = std::move(reason);
    outgoing_.push_back({CURLWS_CLOSE, std::move(payload)});
  }
  void Stop() { stop_.store(true); }

 private:
  // CURLOPT_HEADERFUNCTION sink: capture the negotiated Sec-WebSocket-Protocol
  // from the handshake response. Runs on the worker thread during the perform.
  static size_t OnHeader(char* buffer,
                         size_t size,
                         size_t nitems,
                         void* userdata) {
    size_t len = size * nitems;
    auto* out = static_cast<std::string*>(userdata);
    static const char kName[] = "sec-websocket-protocol:";
    const size_t kNameLen = sizeof(kName) - 1;
    if (len >= kNameLen) {
      bool match = true;
      for (size_t i = 0; i < kNameLen; ++i) {
        char ch = buffer[i];
        if (ch >= 'A' && ch <= 'Z')
          ch = static_cast<char>(ch - 'A' + 'a');
        if (ch != kName[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        size_t start = kNameLen, end = len;
        while (start < end && (buffer[start] == ' ' || buffer[start] == '\t'))
          ++start;
        while (end > start && (buffer[end - 1] == '\r' || buffer[end - 1] == '\n' ||
                               buffer[end - 1] == ' ' || buffer[end - 1] == '\t'))
          --end;
        out->assign(buffer + start, end - start);
      }
    }
    return len;
  }

  // Send one complete WebSocket frame, tolerating the non-blocking socket:
  // curl_ws_send may report CURLE_AGAIN (nothing sent) or a partial `sent`;
  // either way we keep the message and resend the remaining payload (curl
  // continues the same frame once a header has been written). Returns false on
  // a hard send error or if asked to stop mid-frame.
  bool SendWholeFrame(CURL* c,
                      const char* data,
                      size_t size,
                      unsigned int flags) {
    size_t off = 0;
    for (;;) {
      size_t sent = 0;
      CURLcode rc = curl_ws_send(c, data + off, size - off, &sent, 0, flags);
      off += sent;
      if (off >= size)
        return true;  // whole frame flushed (also the size==0 control case)
      if (rc == CURLE_OK || rc == CURLE_AGAIN) {
        if (stop_.load())
          return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;  // resend the remaining bytes of this same frame
      }
      return false;  // hard error
    }
  }

  void ThreadMain() {
    CURL* c = curl_easy_init();
    if (!c) {
      std::move(connected_).Run(false, std::string());
      return;
    }
    struct curl_slist* req_headers = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);  // 2 = WebSocket
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    if (!protocols_.empty()) {
      std::string h = "Sec-WebSocket-Protocol: " + protocols_;
      req_headers = curl_slist_append(req_headers, h.c_str());
      curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);
    }
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, &CurlWsTransport::OnHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &selected_protocol_);
    CURLcode rc = curl_easy_perform(c);  // blocking ws/wss handshake
    if (rc != CURLE_OK) {
      curl_easy_cleanup(c);
      curl_slist_free_all(req_headers);
      std::move(connected_).Run(false, std::string());
      return;
    }
    // Non-blocking socket so curl_ws_recv returns CURLE_AGAIN (vs blocking),
    // letting the loop observe `stop_` promptly.
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock) == CURLE_OK &&
        sock != CURL_SOCKET_BAD) {
#if BUILDFLAG(IS_WIN)
      u_long nonblock = 1;
      ::ioctlsocket(sock, FIONBIO, &nonblock);
#else
      int fl = ::fcntl(sock, F_GETFL, 0);
      if (fl != -1)
        ::fcntl(sock, F_SETFL, fl | O_NONBLOCK);
#endif
    }
    // -> establish + onopen on service thread, with the negotiated subprotocol.
    std::move(connected_).Run(true, selected_protocol_);

    // Close disposition reported to the page; defaults to an abnormal 1006 so
    // an unexpected transport drop is not mislabeled as clean.
    bool was_clean = false;
    uint16_t close_code = 1006;
    std::string close_reason;
    bool close_sent = false;  // did we already transmit a CLOSE frame?

    std::string frag;       // accumulate a partial / multi-frame message
    bool frag_text = true;  // type comes from the first fragment
    bool finished = false;
    while (!stop_.load() && !finished) {
      for (;;) {  // drain queued outgoing messages / page CLOSE
        std::pair<unsigned int, std::string> msg;
        {
          std::lock_guard<std::mutex> g(mu_);
          if (outgoing_.empty())
            break;
          msg = std::move(outgoing_.front());
          outgoing_.pop_front();
        }
        if (!SendWholeFrame(c, msg.second.data(), msg.second.size(),
                            msg.first)) {
          was_clean = false;
          close_code = 1006;
          finished = true;
          break;
        }
        if (msg.first & CURLWS_CLOSE) {  // page-initiated close transmitted
          std::lock_guard<std::mutex> g(mu_);
          was_clean = true;
          close_code = page_close_code_;
          close_reason = page_close_reason_;
          close_sent = true;
          finished = true;
          break;
        }
      }
      if (finished)
        break;
      char buf[16384];
      size_t got = 0;
      const struct curl_ws_frame* meta = nullptr;
      rc = curl_ws_recv(c, buf, sizeof(buf), &got, &meta);
      if (rc == CURLE_OK && meta) {
        if (meta->flags & CURLWS_CLOSE) {
          // Server-initiated close: payload is a 2-byte big-endian status code
          // optionally followed by a UTF-8 reason (RFC6455 §5.5.1).
          was_clean = true;
          if (got >= 2) {
            close_code = static_cast<uint16_t>(
                (static_cast<uint8_t>(buf[0]) << 8) |
                static_cast<uint8_t>(buf[1]));
            close_reason.assign(buf + 2, got - 2);
          } else {
            close_code = 1005;  // "no status received"
          }
          break;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG))
          continue;  // control frame (curl auto-PONGs); never page data
        if (meta->flags & (CURLWS_TEXT | CURLWS_BINARY))
          frag_text = (meta->flags & CURLWS_TEXT) != 0;
        if (got) {
          if (frag.size() + got > kMaxMessageBytes) {
            was_clean = false;
            close_code = 1009;  // message too big
            break;
          }
          frag.append(buf, got);
        }
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
          if (frag_text && !base::IsStringUTF8(frag)) {
            was_clean = false;
            close_code = 1007;  // invalid UTF-8 in a text message
            break;
          }
          message_.Run(frag_text, std::move(frag));
          frag.clear();
        }
        continue;  // drain buffered data before sleeping
      }
      if (rc == CURLE_AGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      // Transport error / connection dropped without a CLOSE frame.
      was_clean = false;
      close_code = 1006;
      break;
    }
    if (!close_sent) {
      // Best-effort CLOSE so the peer sees a shutdown (server-close echo,
      // protocol failure, or local teardown).
      size_t sent = 0;
      curl_ws_send(c, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    curl_easy_cleanup(c);
    curl_slist_free_all(req_headers);
    // -> onclose on the service thread (if alive), with the real disposition.
    std::move(closed_).Run(was_clean, close_code, std::move(close_reason));
  }

  std::string url_;
  std::string protocols_;          // comma-joined requested subprotocols
  std::string selected_protocol_;  // negotiated value, captured at handshake
  ConnectedCb connected_;
  MessageCb message_;
  ClosedCb closed_;
  std::mutex mu_;
  std::deque<std::pair<unsigned int, std::string>> outgoing_;  // guarded by mu_
  uint16_t page_close_code_ = 1000;       // guarded by mu_
  std::string page_close_reason_;         // guarded by mu_
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
  void OnTransportConnected(bool ok, std::string selected_protocol) {
    std::unique_ptr<Pending> p = std::move(pending_establish_);
    if (!p)
      return;
    if (!ok) {
      p->handshake_client->OnFailure("websocket connect failed",
                                     net::ERR_FAILED, -1);
      delete this;  // never established; nothing else references us
      return;
    }
    // Hand the page its endpoints — onopen fires now, on a real connection,
    // reflecting the subprotocol the server negotiated.
    p->handshake_client->OnConnectionEstablished(
        std::move(p->socket_remote), std::move(p->ws_client_receiver),
        MakeHandshake101(p->url, selected_protocol),
        std::move(p->readable_consumer), std::move(p->writable_producer));
  }
  void OnTransportMessage(bool is_text, std::string bytes) {
    DeliverToPage(is_text ? WebSocketMessageType::TEXT
                          : WebSocketMessageType::BINARY,
                  bytes);
  }
  void OnTransportClosed(bool was_clean, uint16_t code, std::string reason) {
    if (client_)
      client_->OnDropChannel(was_clean, code, blink::String::FromUtf8(reason));
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
    if (mode_ == kReal && transport_) {
      // Transmit a CLOSE frame carrying the page's code+reason. The worker
      // sends it, then exits and reports OnDropChannel with the real
      // disposition (was_clean + code/reason) — so we do NOT drop the channel
      // here, which would race a premature/duplicate clean-1000. (We do not
      // block on the server's CLOSE echo: a full round-trip wait would risk
      // hanging this single-threaded model; was_clean stays accurate either
      // way since the page initiated the close.)
      transport_->Close(code, reason.Utf8());
      return;
    }
    // LOOPBACK: no transport, close immediately with the page's code/reason.
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

  // Curl frame flags for an outgoing data frame. blink announces one whole
  // message per SendMessage (always TEXT/BINARY), but a CONTINUATION must stay
  // part of the in-progress message — sent with CURLWS_CONT against the started
  // data type, never as a fresh BINARY frame.
  unsigned int OutgoingFlags(WebSocketMessageType type) {
    switch (type) {
      case WebSocketMessageType::TEXT:
        outgoing_text_ = true;
        return CURLWS_TEXT;
      case WebSocketMessageType::BINARY:
        outgoing_text_ = false;
        return CURLWS_BINARY;
      case WebSocketMessageType::CONTINUATION:
      default:
        return (outgoing_text_ ? CURLWS_TEXT : CURLWS_BINARY) | CURLWS_CONT;
    }
  }

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
          transport_->Send(OutgoingFlags(f.type), std::move(msg));
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
  bool outgoing_text_ = true;   // type of the in-progress outgoing message
  std::shared_ptr<CurlWsTransport> transport_;       // kReal only
  std::unique_ptr<Pending> pending_establish_;       // kReal, until connected
  base::WeakPtrFactory<MbWebSocket> weak_{this};
};

class MbWebSocketConnector : public blink::mojom::blink::WebSocketConnector {
 public:
  void Connect(const blink::KURL& url,
               const blink::Vector<blink::String>& requested_protocols,
               const blink::String& /*user_agent*/,
               net::StorageAccessApiStatus /*storage_access_api_status*/,
               mojo::PendingRemote<WebSocketHandshakeClient> handshake_client,
               const std::optional<base::UnguessableToken>&
               /*throttling_profile_id*/) override {
    mojo::Remote<WebSocketHandshakeClient> client(std::move(handshake_client));

    // Comma-joined requested subprotocols, offered to the server and (for
    // loopback) echoed back as the selected one.
    std::string protocols;
    for (const auto& p : requested_protocols) {
      if (!protocols.empty())
        protocols += ", ";
      protocols += p.Utf8();
    }

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
      // Offline data-plane: establish immediately, echo in-process. Select the
      // first requested subprotocol (a real echo server would pick one).
      std::string selected =
          requested_protocols.empty() ? std::string()
                                      : requested_protocols.front().Utf8();
      client->OnConnectionEstablished(
          std::move(socket_remote), std::move(ws_client_receiver),
          MakeHandshake101(url, selected), std::move(readable_consumer),
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
        url.GetString().Utf8(), std::move(protocols),
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
