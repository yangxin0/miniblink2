// mb_devtools_server.cc — a loopback CDP endpoint (item 41). Sockets on
// dedicated threads; every touch of a view or its CDP bridge is marshaled to
// the engine main thread. See the header for the design contract.
//
// Cross-platform: one implementation over the internal socket-compat layer
// (compat/mb_socket.h — BSD sockets on POSIX/macOS, Winsock2 on Windows). The
// bridge it sits on is already platform-independent.

#include "miniblink_host/devtools/mb_devtools_server.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "build/build_config.h"
#include "compat/mb_socket.h"

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/hash/sha1.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "miniblink_host/view/mb_webview.h"

namespace mb {

#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_POSIX)

// No socket API available: the endpoint is unsupported. The exports still link.
bool MbDevToolsServerStart(int) { return false; }
void MbDevToolsServerStop() {}
int MbDevToolsServerPort() { return 0; }

#else

namespace {

// Socket primitives from the internal compat layer (compat/mb_socket.h).
using compat::kInvalidSocket;
using compat::kShutdownBoth;
using compat::socket_t;
using compat::SocketClose;
using compat::SocketRecv;
using compat::SocketSend;
using compat::SocketSetOptInt;
using compat::SocketValid;

constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Read exactly `n` bytes from `s` into `out`. False on EOF/error.
bool ReadN(socket_t s, char* out, size_t n) {
  size_t got = 0;
  while (got < n) {
    long r = SocketRecv(s, out + got, n - got);
    if (r <= 0)
      return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

bool WriteAll(socket_t s, const char* data, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    long w = SocketSend(s, data + sent, n - sent);
    if (w <= 0)
      return false;
    sent += static_cast<size_t>(w);
  }
  return true;
}

// Read HTTP request headers up to the blank line (bounded). Returns the header
// block (without the trailing CRLFCRLF) or empty on error/oversize.
std::string ReadHttpHeaders(socket_t s) {
  std::string buf;
  char c;
  while (buf.size() < 16 * 1024) {
    long r = SocketRecv(s, &c, 1);
    if (r <= 0)
      return {};
    buf.push_back(c);
    if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0)
      return buf.substr(0, buf.size() - 4);
  }
  return {};
}

std::string HeaderValue(const std::string& headers, const std::string& name) {
  // Case-insensitive line scan for "name:".
  std::string lname = name;
  for (char& ch : lname)
    ch = static_cast<char>(::tolower(ch));
  size_t pos = 0;
  while (pos < headers.size()) {
    size_t eol = headers.find("\r\n", pos);
    std::string line = headers.substr(
        pos, eol == std::string::npos ? std::string::npos : eol - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (char& ch : key)
        ch = static_cast<char>(::tolower(ch));
      if (key == lname) {
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
          v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
          v.pop_back();
        return v;
      }
    }
    if (eol == std::string::npos)
      break;
    pos = eol + 2;
  }
  return {};
}

// Minimal JSON string escaping for URLs/titles in /json output.
std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

void SendHttp(socket_t s, const std::string& status, const std::string& ctype,
              const std::string& body) {
  std::string resp = "HTTP/1.1 " + status + "\r\n";
  resp += "Content-Type: " + ctype + "\r\n";
  resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  resp += "Connection: close\r\n\r\n";
  resp += body;
  WriteAll(s, resp.data(), resp.size());
}

// Format/parse the opaque target id — the view pointer as hex. Validated
// against the live-view set on the main thread before any dereference.
std::string TargetIdFor(MbWebView* v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v)));
  return buf;
}
MbWebView* TargetFromId(const std::string& id) {
  if (id.empty())
    return nullptr;
  unsigned long long val = 0;
  if (std::sscanf(id.c_str(), "%llx", &val) != 1)
    return nullptr;
  return reinterpret_cast<MbWebView*>(static_cast<uintptr_t>(val));
}

}  // namespace

// One connection's shared state: the socket plus a send lock (the main-thread
// CDP callback and the shutdown path both write/close it).
struct MbDevToolsConn {
  socket_t fd = kInvalidSocket;
  std::mutex send_lock;
  bool closed = false;

  // Send one server->client WebSocket TEXT frame (unmasked).
  void SendText(const std::string& payload) {
    std::lock_guard<std::mutex> l(send_lock);
    if (closed)
      return;
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text
    const size_t n = payload.size();
    if (n < 126) {
      frame.push_back(static_cast<char>(n));
    } else if (n < 65536) {
      frame.push_back(126);
      frame.push_back(static_cast<char>((n >> 8) & 0xff));
      frame.push_back(static_cast<char>(n & 0xff));
    } else {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i)
        frame.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
    }
    frame += payload;
    WriteAll(fd, frame.data(), frame.size());
  }
};

class MbDevToolsServer {
 public:
  static MbDevToolsServer* Get() {
    static MbDevToolsServer* s = new MbDevToolsServer();  // leaked; process life
    return s;
  }

  bool Start(int port) {
    if (running_) {
      return port_ == port;  // idempotent on the same port
    }
    if (!compat::SocketPlatformInit())
      return false;
    main_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    if (!main_runner_)
      return false;

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!SocketValid(fd))
      return false;
    SocketSetOptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 8) != 0) {
      SocketClose(fd);
      return false;
    }
    listen_fd_ = fd;
    port_ = port;
    running_ = true;
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  void Stop() {
    if (!running_)
      return;
    running_ = false;
    if (SocketValid(listen_fd_)) {
      ::shutdown(listen_fd_, kShutdownBoth);
      SocketClose(listen_fd_);
      listen_fd_ = kInvalidSocket;
    }
    // Unblock all connection reads by closing their sockets.
    {
      std::lock_guard<std::mutex> l(conns_lock_);
      for (const auto& c : conns_) {
        std::lock_guard<std::mutex> sl(c->send_lock);
        c->closed = true;
        if (SocketValid(c->fd))
          ::shutdown(c->fd, kShutdownBoth);
      }
    }
    if (accept_thread_.joinable())
      accept_thread_.join();
    port_ = 0;
  }

  int port() const { return running_ ? port_ : 0; }

 private:
  void AcceptLoop() {
    while (running_) {
      socket_t cfd = ::accept(listen_fd_, nullptr, nullptr);
      if (!SocketValid(cfd)) {
        if (!running_)
          break;
        continue;
      }
      SocketSetOptInt(cfd, IPPROTO_TCP, TCP_NODELAY, 1);
      std::thread([this, cfd] { HandleConnection(cfd); }).detach();
    }
  }

  // Post `task` to the main thread and block until it has run.
  void RunOnMainSync(base::OnceClosure task) {
    if (main_runner_->BelongsToCurrentThread()) {
      std::move(task).Run();
      return;
    }
    base::WaitableEvent ev;
    main_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::OnceClosure t, base::WaitableEvent* e) {
              std::move(t).Run();
              e->Signal();
            },
            std::move(task), &ev));
    ev.Wait();
  }

  void HandleConnection(socket_t fd) {
    std::string headers = ReadHttpHeaders(fd);
    if (headers.empty()) {
      SocketClose(fd);
      return;
    }
    // Request line: "GET <path> HTTP/1.1".
    size_t sp1 = headers.find(' ');
    size_t sp2 = headers.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    std::string path =
        (sp1 != std::string::npos && sp2 != std::string::npos)
            ? headers.substr(sp1 + 1, sp2 - sp1 - 1)
            : std::string();

    const std::string upgrade = HeaderValue(headers, "upgrade");
    bool is_ws = false;
    for (char c : upgrade)
      if (::tolower(c) == 'w') {
        is_ws = (upgrade.size() >= 9);  // "websocket"
        break;
      }

    if (is_ws && path.rfind("/devtools/page/", 0) == 0) {
      HandleWebSocket(fd, headers, path.substr(std::strlen("/devtools/page/")));
      return;
    }
    if (path == "/json/version") {
      ServeVersion(fd);
    } else if (path == "/json" || path == "/json/list") {
      ServeList(fd);
    } else {
      SendHttp(fd, "404 Not Found", "text/plain", "not found");
    }
    SocketClose(fd);
  }

  void ServeVersion(socket_t fd) {
    std::string body =
        "{\n  \"Browser\": \"miniblink2\",\n"
        "  \"Protocol-Version\": \"1.3\"\n}";
    SendHttp(fd, "200 OK", "application/json; charset=UTF-8", body);
  }

  void ServeList(socket_t fd) {
    // Snapshot targets on the main thread (touches blink: URL/title).
    struct Target {
      std::string id, url, title;
    };
    std::vector<Target> targets;
    const int port = port_;
    RunOnMainSync(base::BindOnce(
        [](std::vector<Target>* out) {
          for (MbWebView* v : MbEnumerateViews()) {
            Target t;
            t.id = TargetIdFor(v);
            t.url = v->GetURL();
            t.title = v->GetTitle();
            out->push_back(std::move(t));
          }
        },
        &targets));

    std::string body = "[";
    for (size_t i = 0; i < targets.size(); ++i) {
      const Target& t = targets[i];
      const std::string ws =
          "127.0.0.1:" + std::to_string(port) + "/devtools/page/" + t.id;
      if (i)
        body += ",";
      body +=
          "\n {\n"
          "  \"id\": \"" + t.id + "\",\n"
          "  \"type\": \"page\",\n"
          "  \"title\": \"" + JsonEscape(t.title) + "\",\n"
          "  \"url\": \"" + JsonEscape(t.url) + "\",\n"
          "  \"webSocketDebuggerUrl\": \"ws://" + ws + "\",\n"
          "  \"devtoolsFrontendUrl\": "
          "\"devtools://devtools/bundled/inspector.html?ws=" + ws + "\"\n"
          " }";
    }
    body += targets.empty() ? "]" : "\n]";
    SendHttp(fd, "200 OK", "application/json; charset=UTF-8", body);
  }

  void HandleWebSocket(socket_t fd, const std::string& headers,
                       const std::string& target_id) {
    const std::string key = HeaderValue(headers, "sec-websocket-key");
    if (key.empty()) {
      SendHttp(fd, "400 Bad Request", "text/plain", "missing key");
      SocketClose(fd);
      return;
    }
    // Handshake: Sec-WebSocket-Accept = base64(sha1(key + GUID)).
    const std::string accept =
        base::Base64Encode(base::SHA1HashString(key + kWsGuid));
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!WriteAll(fd, resp.data(), resp.size())) {
      SocketClose(fd);
      return;
    }

    auto conn = std::make_shared<MbDevToolsConn>();
    conn->fd = fd;
    MbWebView* view = TargetFromId(target_id);
    std::weak_ptr<MbDevToolsConn> weak = conn;

    // Attach the view's CDP session on the main thread; the message callback
    // (also main thread) writes frames to this connection.
    bool attached = false;
    RunOnMainSync(base::BindOnce(
        [](MbWebView* v, std::weak_ptr<MbDevToolsConn> weak, bool* ok) {
          if (!MbIsLiveView(v))
            return;
          *ok = v->AttachDevTools([weak](const std::string& msg) {
            if (auto c = weak.lock())
              c->SendText(msg);
          });
        },
        view, weak, &attached));

    if (!attached) {
      // Already attached elsewhere, or the view is gone — close cleanly.
      std::lock_guard<std::mutex> l(conn->send_lock);
      conn->closed = true;
      SocketClose(fd);
      return;
    }

    RegisterConn(conn);
    ReadLoop(conn, view);
    UnregisterConn(conn);

    // Detach on the main thread (no-op if the view is already gone).
    RunOnMainSync(base::BindOnce(
        [](MbWebView* v) {
          if (MbIsLiveView(v))
            v->DetachDevTools();
        },
        view));
    {
      std::lock_guard<std::mutex> l(conn->send_lock);
      conn->closed = true;
    }
    SocketClose(fd);
  }

  // Read client->server frames; deliver each complete text message to the
  // view's CDP session on the main thread. Returns when the socket closes.
  void ReadLoop(const std::shared_ptr<MbDevToolsConn>& conn, MbWebView* view) {
    const socket_t fd = conn->fd;
    std::string message;  // reassembled across continuation frames
    for (;;) {
      char h[2];
      if (!ReadN(fd, h, 2))
        return;
      const bool fin = (h[0] & 0x80) != 0;
      const int opcode = h[0] & 0x0f;
      const bool masked = (h[1] & 0x80) != 0;
      uint64_t len = h[1] & 0x7f;
      if (len == 126) {
        char e[2];
        if (!ReadN(fd, e, 2))
          return;
        len = (static_cast<uint8_t>(e[0]) << 8) | static_cast<uint8_t>(e[1]);
      } else if (len == 127) {
        char e[8];
        if (!ReadN(fd, e, 8))
          return;
        len = 0;
        for (int i = 0; i < 8; ++i)
          len = (len << 8) | static_cast<uint8_t>(e[i]);
      }
      char mask[4] = {0, 0, 0, 0};
      if (masked && !ReadN(fd, mask, 4))
        return;
      if (len > 64 * 1024 * 1024)
        return;  // absurd frame; drop the connection
      std::string payload(static_cast<size_t>(len), '\0');
      if (len && !ReadN(fd, payload.data(), payload.size()))
        return;
      if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
          payload[i] ^= mask[i & 3];

      if (opcode == 0x8)  // close
        return;
      if (opcode == 0x9) {  // ping -> pong
        SendControl(conn, 0x0a, payload);
        continue;
      }
      if (opcode == 0x0a)  // pong
        continue;
      // 0x0 continuation, 0x1 text, 0x2 binary (CDP is text).
      message += payload;
      if (!fin)
        continue;
      std::string cdp = std::move(message);
      message.clear();
      if (cdp.empty())
        continue;
      RunOnMainSync(base::BindOnce(
          [](MbWebView* v, std::string json) {
            if (MbIsLiveView(v))
              v->SendDevTools(json.data(), static_cast<int>(json.size()));
          },
          view, std::move(cdp)));
    }
  }

  void SendControl(const std::shared_ptr<MbDevToolsConn>& conn, int opcode,
                   const std::string& payload) {
    std::lock_guard<std::mutex> l(conn->send_lock);
    if (conn->closed)
      return;
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    frame.push_back(static_cast<char>(payload.size() & 0x7f));
    frame += payload;
    WriteAll(conn->fd, frame.data(), frame.size());
  }

  void RegisterConn(const std::shared_ptr<MbDevToolsConn>& c) {
    std::lock_guard<std::mutex> l(conns_lock_);
    conns_.push_back(c);
  }
  void UnregisterConn(const std::shared_ptr<MbDevToolsConn>& c) {
    std::lock_guard<std::mutex> l(conns_lock_);
    for (auto it = conns_.begin(); it != conns_.end(); ++it)
      if (*it == c) {
        conns_.erase(it);
        break;
      }
  }

  std::atomic<bool> running_{false};
  socket_t listen_fd_ = kInvalidSocket;
  int port_ = 0;
  std::thread accept_thread_;
  scoped_refptr<base::SingleThreadTaskRunner> main_runner_;
  std::mutex conns_lock_;
  std::vector<std::shared_ptr<MbDevToolsConn>> conns_;
};

bool MbDevToolsServerStart(int port) {
  return MbDevToolsServer::Get()->Start(port);
}
void MbDevToolsServerStop() {
  MbDevToolsServer::Get()->Stop();
}
int MbDevToolsServerPort() {
  return MbDevToolsServer::Get()->port();
}

#endif  // socket API available

}  // namespace mb
