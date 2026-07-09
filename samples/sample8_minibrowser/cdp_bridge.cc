// cdp_bridge.cc — implementation of the loopback CDP endpoint (cdp_bridge.h).
// Portable C++: BSD/winsock sockets, a self-contained SHA-1 + base64 for the
// WebSocket handshake, engine hops via the compat scaffold's MbPostToMain.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <BaseTsd.h>
typedef SSIZE_T ssize_t;   // MSVC has no ssize_t
using socket_t = SOCKET;
static void CloseSock(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t INVALID_SOCKET = -1;
static void CloseSock(socket_t s) { close(s); }
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../compat/mb_window.h"
#include "cdp_bridge.h"

namespace {

// ---- SHA-1 (RFC 3174) + base64 — just enough for Sec-WebSocket-Accept ---------

void Sha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    std::vector<unsigned char> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; --i) msg.push_back((unsigned char)(bits >> (i * 8)));
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)msg[chunk + i * 4] << 24 |
                   (uint32_t)msg[chunk + i * 4 + 1] << 16 |
                   (uint32_t)msg[chunk + i * 4 + 2] << 8 |
                   (uint32_t)msg[chunk + i * 4 + 3];
        for (int i = 16; i < 80; ++i) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = (unsigned char)(h[i] >> ((3 - j) * 8));
}

std::string Base64(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < len ? tbl[v & 63] : '=';
    }
    return out;
}

// ---- State ---------------------------------------------------------------------

struct Target {
    std::string id;     // "page-N", the /devtools/page/<id> path component
    std::string title;
    std::string url;
};

std::mutex g_lock;
socket_t g_listen_fd = INVALID_SOCKET;
int g_port = 0;
std::map<mbView*, Target> g_targets;
int g_next_id = 1;
socket_t g_client_fd = INVALID_SOCKET;   // the live WebSocket
mbView* g_client_view = nullptr;         // the target it is bound to

// Run a C++ closure on the main thread's frame tick (MbPostToMain shim).
void RunOnMain(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    MbPostToMain([](void* ud) {
        auto* fn = static_cast<std::function<void()>*>(ud);
        (*fn)();
        delete fn;
    }, p);
}

// ---- WebSocket framing (RFC 6455) ------------------------------------------------

void SendAll(socket_t fd, const char* data, size_t n) {
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
        int w = send(fd, data + off, (int)(n - off), 0);
#else
        ssize_t w = write(fd, data + off, n - off);
#endif
        if (w <= 0) break;
        off += (size_t)w;
    }
}

// One unmasked single-fragment frame (caller holds g_lock).
void SendFrameLocked(socket_t fd, uint8_t opcode, const char* payload, size_t n) {
    std::vector<char> frame;
    frame.reserve(n + 10);
    frame.push_back((char)(0x80 | opcode));
    if (n < 126) {
        frame.push_back((char)n);
    } else if (n <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((char)(n >> 8));
        frame.push_back((char)(n & 0xFF));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back((char)((n >> shift) & 0xFF));
    }
    frame.insert(frame.end(), payload, payload + n);
    SendAll(fd, frame.data(), frame.size());
}

// Engine -> wire: a CDP response/notification (fires on the main thread).
void OnDevToolsMessage(mbView* v, void*, const char* json, int len) {
    std::lock_guard<std::mutex> al(g_lock);
    if (g_client_fd != INVALID_SOCKET && g_client_view == v && json && len > 0)
        SendFrameLocked(g_client_fd, 0x1, json, (size_t)len);
}

// One complete client frame at the head of `buf` -> bytes consumed (0 = need
// more), opcode, unmasked payload.
size_t DecodeFrame(const std::vector<uint8_t>& buf, uint8_t* opcode,
                   std::string* payload) {
    if (buf.size() < 2) return 0;
    *opcode = buf[0] & 0x0F;
    const bool masked = (buf[1] & 0x80) != 0;
    size_t len = buf[1] & 0x7F, idx = 2;
    if (len == 126) {
        if (buf.size() < 4) return 0;
        len = ((size_t)buf[2] << 8) | buf[3];
        idx = 4;
    } else if (len == 127) {
        if (buf.size() < 10) return 0;
        len = 0;
        for (int i = 2; i < 10; ++i) len = (len << 8) | buf[i];
        idx = 10;
    }
    uint8_t mask[4] = {0};
    if (masked) {
        if (buf.size() < idx + 4) return 0;
        memcpy(mask, buf.data() + idx, 4);
        idx += 4;
    }
    if (buf.size() < idx + len) return 0;
    payload->assign((const char*)buf.data() + idx, len);
    if (masked)
        for (size_t i = 0; i < len; ++i) (*payload)[i] ^= (char)mask[i % 4];
    return idx + len;
}

// ---- HTTP ------------------------------------------------------------------------

void HttpText(socket_t fd, const char* status, const std::string& body,
              const char* content_type) {
    char head[256];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Connection: close\r\n\r\n",
                  status, content_type, body.size());
    std::string out = std::string(head) + body;
    SendAll(fd, out.data(), out.size());
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

// One entry per registered tab — chrome://inspect renders these.
std::string JsonList() {
    std::lock_guard<std::mutex> al(g_lock);
    std::string out = "[";
    bool first = true;
    for (const auto& [view, t] : g_targets) {
        (void)view;
        char ws[128];
        std::snprintf(ws, sizeof ws, "127.0.0.1:%d/devtools/page/%s", g_port,
                      t.id.c_str());
        if (!first) out += ",";
        first = false;
        out += "{\"description\":\"minibrowser tab\",\"id\":\"" + t.id +
               "\",\"type\":\"page\",\"title\":\"" + JsonEscape(t.title) +
               "\",\"url\":\"" + JsonEscape(t.url) +
               "\",\"webSocketDebuggerUrl\":\"ws://" + ws +
               "\",\"devtoolsFrontendUrl\":"
               "\"devtools://devtools/bundled/inspector.html?ws=" +
               std::string(ws) + "\"}";
    }
    return out + "]";
}

// Advertise the REAL Chromium version: chrome://inspect resolves its hosted
// frontend from this; an unknown product makes the "inspect" link a no-op.
std::string JsonVersion() {
    return std::string("{\"Browser\":\"Chrome/") + mbChromiumVersion() +
           "\",\"Protocol-Version\":\"1.3\","
           "\"User-Agent\":\"minibrowser (miniblink2)\","
           "\"WebKit-Version\":\"537.36 (miniblink2)\"}";
}

// Sec-WebSocket-Accept: base64(SHA1(key + RFC6455 GUID)).
std::string WsAccept(const std::string& key) {
    const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20];
    Sha1((const unsigned char*)magic.data(), magic.size(), sha);
    return Base64(sha, sizeof sha);
}

// ---- Per-connection thread ---------------------------------------------------------

ssize_t ReadSome(socket_t fd, char* buf, size_t cap) {
#ifdef _WIN32
    return recv(fd, buf, (int)cap, 0);
#else
    return read(fd, buf, cap);
#endif
}

// The WebSocket read loop: client frames -> CDP commands to the bound view.
void WsLoop(socket_t fd, mbView* view) {
    {
        std::lock_guard<std::mutex> al(g_lock);
        if (g_client_fd != INVALID_SOCKET) CloseSock(g_client_fd);  // newest wins
        g_client_fd = fd;
        g_client_view = view;
    }
    std::vector<uint8_t> buf;
    char chunk[16 * 1024];
    for (;;) {
        ssize_t n = ReadSome(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        buf.insert(buf.end(), chunk, chunk + n);
        for (;;) {
            uint8_t opcode = 0;
            std::string payload;
            size_t consumed = DecodeFrame(buf, &opcode, &payload);
            if (!consumed) break;
            buf.erase(buf.begin(), buf.begin() + (long)consumed);
            if (opcode == 0x1) {           // text: one CDP command
                std::string cmd = std::move(payload);
                RunOnMain([fd, view, cmd] {
                    // Validity under the lock, the ENGINE call outside it.
                    // unregister also runs on main, so a stale view fails here.
                    bool bound;
                    {
                        std::lock_guard<std::mutex> al(g_lock);
                        bound = g_client_view == view && g_client_fd == fd;
                    }
                    if (bound)
                        mbDevToolsSend(view, cmd.data(), (int)cmd.size());
                });
            } else if (opcode == 0x9) {    // ping -> pong
                std::lock_guard<std::mutex> al(g_lock);
                if (g_client_fd == fd)
                    SendFrameLocked(fd, 0xA, payload.data(), payload.size());
            } else if (opcode == 0x8) {    // close
                goto out;
            }
        }
    }
out:
    std::lock_guard<std::mutex> al(g_lock);
    if (g_client_fd == fd) {
        g_client_fd = INVALID_SOCKET;
        g_client_view = nullptr;
    }
    CloseSock(fd);
}

void Connection(socket_t fd) {
    std::string head;
    char c[2048];
    while (head.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ReadSome(fd, c, sizeof c);
        if (n <= 0) { CloseSock(fd); return; }
        head.append(c, (size_t)n);
        if (head.size() > 64 * 1024) { CloseSock(fd); return; }
    }
    // Path, query stripped: chrome://inspect asks "/json/list?for_tab".
    size_t s1 = head.find(' '), s2 = head.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) {
        CloseSock(fd);
        return;
    }
    std::string path = head.substr(s1 + 1, s2 - s1 - 1);
    if (size_t q = path.find('?'); q != std::string::npos) path.resize(q);

    auto header = [&head](const char* name) -> std::string {
        std::string lower = head;
        for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
        std::string needle = std::string("\r\n") + name + ":";
        size_t p = lower.find(needle);
        if (p == std::string::npos) return "";
        p += needle.size();
        size_t e = head.find("\r\n", p);
        std::string v = head.substr(p, e - p);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
    };

    if (header("upgrade").find("ebsocket") != std::string::npos) {
        const std::string key = header("sec-websocket-key");
        const char kPrefix[] = "/devtools/page/";
        std::string id =
            path.rfind(kPrefix, 0) == 0 ? path.substr(sizeof kPrefix - 1) : "";
        mbView* view = nullptr;
        {
            std::lock_guard<std::mutex> al(g_lock);
            for (const auto& [v, t] : g_targets)
                if (t.id == id) { view = v; break; }
        }
        if (!view || key.empty()) { CloseSock(fd); return; }
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
            WsAccept(key) + "\r\n\r\n";
        SendAll(fd, resp.data(), resp.size());
        WsLoop(fd, view);   // runs until the socket dies
        return;
    }
    if (path == "/json" || path == "/json/list") {
        HttpText(fd, "200 OK", JsonList(), "application/json");
    } else if (path == "/json/version") {
        HttpText(fd, "200 OK", JsonVersion(), "application/json");
    } else {
        HttpText(fd, "404 Not Found", "", "text/plain");
    }
    CloseSock(fd);
}

}  // namespace

bool MbBridgeStart(int port) {
    std::lock_guard<std::mutex> al(g_lock);
    if (g_listen_fd != INVALID_SOCKET) return true;   // already running
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // idempotent (refcounted)
#endif
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        CloseSock(fd);
        return false;
    }
    g_listen_fd = fd;
    g_port = port;
    std::thread([fd] {
        for (;;) {
            socket_t client = accept(fd, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;   // listen socket closed
            std::thread(Connection, client).detach();
        }
    }).detach();
    std::fprintf(stderr,
                 "[minibrowser] DevTools endpoint on http://127.0.0.1:%d "
                 "(attach via chrome://inspect)\n", port);
    return true;
}

void MbBridgeRegister(mbView* view, const std::string& title,
                      const std::string& url) {
    if (!view) return;
    {
        std::lock_guard<std::mutex> al(g_lock);
        auto it = g_targets.find(view);
        if (it != g_targets.end()) {   // refresh metadata only
            it->second.title = title;
            it->second.url = url;
            return;
        }
    }
    // Attach the engine CDP session (main thread, engine off the stack) BEFORE
    // advertising the target, so it is warm when a frontend connects.
    if (!mbDevToolsAttach(view, OnDevToolsMessage, nullptr)) return;
    std::lock_guard<std::mutex> al(g_lock);
    char id[32];
    std::snprintf(id, sizeof id, "page-%d", g_next_id++);
    g_targets[view] = Target{id, title, url};
}

void MbBridgeUnregister(mbView* view) {
    bool was_attached = false;
    {
        std::lock_guard<std::mutex> al(g_lock);
        auto it = g_targets.find(view);
        if (it == g_targets.end()) return;
        g_targets.erase(it);
        was_attached = true;
        if (g_client_view == view) {   // frontend was bound to this tab
            if (g_client_fd != INVALID_SOCKET) CloseSock(g_client_fd);
            g_client_fd = INVALID_SOCKET;
            g_client_view = nullptr;
        }
    }
    if (was_attached) mbDevToolsDetach(view);
}
