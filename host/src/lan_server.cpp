// LAN sharing server: see lan_server.h for the routes and the security model.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#  define VP_INVALID_SOCKET INVALID_SOCKET
#  define VP_CLOSE closesocket
#  define VP_SHUT_BOTH SD_BOTH
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define VP_INVALID_SOCKET (-1)
#  define VP_CLOSE ::close
#  define VP_SHUT_BOTH SHUT_RDWR
#endif

#include "lan_server.h"
#include "json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace vp::lan {
namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string RandomHex(std::size_t bytes) {
    static std::mutex mutex;
    static std::mt19937_64 engine{std::random_device{}() ^ static_cast<std::uint64_t>(NowMs())};
    std::lock_guard<std::mutex> lock(mutex);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto v = static_cast<unsigned>(engine() & 0xFF);
        out += digits[v >> 4];
        out += digits[v & 15];
    }
    return out;
}

std::string RandomPin() {
    std::random_device device;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string pin;
    for (int i = 0; i < 6; ++i) pin += static_cast<char>('0' + digit(device));
    return pin;
}

std::string Extension(const fs::path& path) { return Lower(PathToUtf8(path.extension())); }

bool IsVideoExt(const std::string& ext) {
    static const char* kExt[] = {".mkv", ".mp4", ".m4v", ".mov", ".avi", ".wmv", ".ts", ".m2ts", ".mts", ".webm", ".flv", ".mpg", ".mpeg", ".vob", ".rmvb"};
    return std::any_of(std::begin(kExt), std::end(kExt), [&](const char* e) { return ext == e; });
}
bool IsSubtitleExt(const std::string& ext) { return ext == ".srt" || ext == ".ass" || ext == ".ssa" || ext == ".vtt" || ext == ".sup"; }
bool IsMetaExt(const std::string& ext) { return ext == ".nfo" || ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".tbn"; }
bool IsFontExt(const std::string& ext) { return ext == ".ttf" || ext == ".otf" || ext == ".ttc"; }

std::string MimeFor(const fs::path& path) {
    const std::string ext = Extension(path);
    static const std::map<std::string, std::string> kMime = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"}, {".json", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json"},
        {".wasm", "application/wasm"}, {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".tbn", "image/jpeg"}, {".webp", "image/webp"}, {".gif", "image/gif"}, {".ico", "image/x-icon"}, {".woff2", "font/woff2"},
        {".woff", "font/woff"}, {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".ttc", "font/collection"}, {".nfo", "text/xml; charset=utf-8"},
        {".mp4", "video/mp4"}, {".m4v", "video/mp4"}, {".mkv", "video/x-matroska"}, {".webm", "video/webm"}, {".mov", "video/quicktime"},
        {".ts", "video/mp2t"}, {".m2ts", "video/mp2t"}, {".mts", "video/mp2t"}, {".srt", "text/plain; charset=utf-8"},
        {".ass", "text/plain; charset=utf-8"}, {".ssa", "text/plain; charset=utf-8"}, {".vtt", "text/vtt; charset=utf-8"},
        {".sup", "application/octet-stream"}, {".txt", "text/plain; charset=utf-8"}};
    auto it = kMime.find(ext);
    return it == kMime.end() ? "application/octet-stream" : it->second;
}

const char* StatusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

// A relative URL path ("a/b/c.png") turned into a path below base; empty when it escapes.
fs::path SafeJoin(const fs::path& base, const std::string& rel_utf8) {
    if (base.empty() || rel_utf8.empty()) return {};
    fs::path rel;
    std::stringstream parts(rel_utf8);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find('\\') != std::string::npos || part.find(':') != std::string::npos || part.find('\0') != std::string::npos)
            return {};
        rel /= PathFromUtf8(part);
    }
    if (rel.empty()) return {};
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(base / rel, ec);
    const fs::path root = fs::weakly_canonical(base, ec);
    const std::string a = Lower(PathToUtf8(full)), b = Lower(PathToUtf8(root));
    if (a.size() <= b.size() || a.compare(0, b.size(), b) != 0) return {};
    return full;
}

bool ParseRange(const std::string& header, std::uint64_t size, std::uint64_t* first, std::uint64_t* last) {
    // Only the first range of "bytes=a-b", "bytes=a-" or "bytes=-n".
    if (header.rfind("bytes=", 0) != 0 || size == 0) return false;
    std::string spec = header.substr(6);
    const auto comma = spec.find(',');
    if (comma != std::string::npos) spec.resize(comma);
    const auto dash = spec.find('-');
    if (dash == std::string::npos) return false;
    const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
    try {
        if (a.empty()) {
            if (b.empty()) return false;
            const std::uint64_t n = std::stoull(b);
            if (n == 0) return false;
            *first = n >= size ? 0 : size - n;
            *last = size - 1;
        } else {
            *first = std::stoull(a);
            *last = b.empty() ? size - 1 : std::min<std::uint64_t>(std::stoull(b), size - 1);
        }
    } catch (...) {
        return false;
    }
    return *first < size && *first <= *last;
}

std::vector<std::string> LocalAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    char name[256] = {};
    if (gethostname(name, sizeof name) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* list = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &list) == 0) {
            for (auto* p = list; p; p = p->ai_next) {
                char text[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, text, sizeof text);
                out.emplace_back(text);
            }
            freeaddrinfo(list);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (auto* p = list; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, text, sizeof text);
            out.emplace_back(text);
        }
        freeifaddrs(list);
    }
#endif
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& a) { return a.rfind("127.", 0) == 0 || !IsPrivateIPv4(a); }), out.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct WinsockInit {
    WinsockInit() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
};
void EnsureSockets() { static WinsockInit init; }

void SetTimeouts(socket_t s, int ms) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof value);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof value);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

class SocketConnection final : public Connection {
public:
    explicit SocketConnection(socket_t s) : socket_(s) {}
    bool Send(const void* data, std::size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size && !closed_) {
            const int chunk = static_cast<int>(std::min<std::size_t>(size, 1 << 20));
#ifdef _WIN32
            const int sent = ::send(socket_, p, chunk, 0);
#else
            const int sent = static_cast<int>(::send(socket_, p, static_cast<std::size_t>(chunk), MSG_NOSIGNAL));
#endif
            if (sent <= 0) { closed_ = true; return false; }
            p += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return !closed_;
    }
private:
    socket_t socket_;
};

} // namespace

// ---------------------------------------------------------------- public helpers
fs::path PathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string PercentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (c == '+') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string PercentEncode(const std::string& text) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += digits[c >> 4]; out += digits[c & 15]; }
    }
    return out;
}

bool IsPrivateIPv4(const std::string& dotted) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(dotted.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

bool Connection::BeginChunked(int status, const std::string& content_type, const std::string& extra_headers) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\nContent-Type: " + content_type +
                       "\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra_headers + "\r\n";
    return SendText(head);
}

bool Connection::SendChunk(const void* data, std::size_t size) {
    if (!size) return true;
    char head[24];
    std::snprintf(head, sizeof head, "%zx\r\n", size);
    return SendText(head) && Send(data, size) && SendText("\r\n");
}

bool Connection::EndChunked() { return SendText("0\r\n\r\n"); }

// ---------------------------------------------------------------- server state
struct Session {
    std::uint64_t id = 0;
    std::string token;
    std::string user_id = "default";
    std::deque<std::string> queue;
    bool alive = true;
    std::condition_variable wake;
};

struct ClientRecord {
    std::string token;
    std::string device;
    std::string peer;
    std::string user_id = "default";
    std::int64_t created = 0;
    std::int64_t last = 0;
};

struct Server::Impl {
    LanHooks hooks;
    std::uint16_t port = 0;
    std::atomic<bool> running{false};
    socket_t listener = VP_INVALID_SOCKET;
    socket_t udp = VP_INVALID_SOCKET;
    std::thread accept_thread;
    std::thread udp_thread;

    mutable std::mutex mutex;
    std::string pin;
    std::string server_id;
    std::vector<ClientRecord> clients;
    std::map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::uint64_t next_session = 1;
    std::set<socket_t> open_sockets;
    int pin_failures = 0;
    std::int64_t pin_locked_until = 0;
    std::atomic<int> active_connections{0};

    // ---- persistence (data\lan_clients.json)
    fs::path StorePath() const { return hooks.data_dir / "lan_clients.json"; }
    void Load() {
        std::ifstream in(StorePath(), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        bool ok = false;
        const vp::Json json = vp::Json::Parse(buffer.str(), &ok);
        server_id = ok ? json["serverId"].str() : std::string();
        if (server_id.empty()) server_id = RandomHex(8);
        clients.clear();
        if (ok)
            for (const vp::Json& c : json["clients"].items()) {
                ClientRecord r;
                r.token = c["token"].str();
                r.device = c["device"].str();
                r.peer = c["peer"].str();
                r.user_id = c["userId"].str("default");
                if (r.user_id.empty() || r.user_id.size() > 32 ||
                    !std::all_of(r.user_id.begin(), r.user_id.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '-'; }))
                    r.user_id = "default";
                r.created = c["created"].i64();
                r.last = c["last"].i64();
                if (r.token.size() == 32) clients.push_back(r);
            }
    }
    void SaveLocked() const {
        std::string out = "{\"serverId\":" + vp::Json::Quote(server_id) + ",\"clients\":[";
        for (std::size_t i = 0; i < clients.size(); ++i) {
            const auto& c = clients[i];
            out += std::string(i ? "," : "") + "{\"token\":" + vp::Json::Quote(c.token) + ",\"device\":" + vp::Json::Quote(c.device) +
                   ",\"peer\":" + vp::Json::Quote(c.peer) + ",\"userId\":" + vp::Json::Quote(c.user_id) + ",\"created\":" + std::to_string(c.created) + ",\"last\":" + std::to_string(c.last) + "}";
        }
        out += "]}\n";
        std::error_code ec;
        fs::create_directories(hooks.data_dir, ec);
        const fs::path temp = StorePath().string() + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file << out;
        }
        fs::rename(temp, StorePath(), ec);
    }

    bool Authorized(Request& request) {
#ifndef VP_LAN_STRICT_LOOPBACK
        if (request.peer.rfind("127.", 0) == 0) { request.user_id = "default"; return true; } // the server PC itself
#endif
        if (request.token.size() != 32) return false;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& c : clients)
            if (c.token == request.token) {
                const std::int64_t now = NowMs();
                if (now - c.last > 60000) { c.last = now; c.peer = request.peer; SaveLocked(); }
                request.user_id = c.user_id.empty() ? "default" : c.user_id;
                return true;
            }
        return false;
    }

    void Log(const std::string& message) const {
        if (hooks.log) hooks.log(message);
    }

    // ---- responses
    static bool Respond(Connection& conn, int status, const std::string& type, const std::string& body, bool keep_alive,
                        const std::string& extra = {}, bool head_only = false) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
        if (!type.empty()) head += "Content-Type: " + type + "\r\n";
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        head += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += extra + "\r\n";
        if (!conn.SendText(head)) return false;
        return head_only || conn.SendText(body);
    }
    static bool Json(Connection& conn, int status, const std::string& body, bool keep_alive, const std::string& extra = {}) {
        return Respond(conn, status, "application/json; charset=utf-8", body, keep_alive, "Cache-Control: no-store\r\n" + extra);
    }
    static bool Error(Connection& conn, int status, bool keep_alive) {
        return Json(conn, status, "{\"error\":" + std::to_string(status) + "}", keep_alive);
    }

    bool SendFile(Connection& conn, const Request& request, const fs::path& file, bool keep_alive, const std::string& cache_control) {
        std::error_code ec;
        const std::uint64_t size = fs::file_size(file, ec);
        if (ec) return Error(conn, 404, keep_alive);
        std::ifstream in(file, std::ios::binary);
        if (!in) return Error(conn, 404, keep_alive);
        const bool head_only = request.method == "HEAD";
        std::uint64_t first = 0, last = size ? size - 1 : 0;
        int status = 200;
        std::string extra = "Accept-Ranges: bytes\r\nCache-Control: " + cache_control + "\r\n";
        auto range = request.headers.find("range");
        if (range != request.headers.end()) {
            if (!ParseRange(range->second, size, &first, &last)) {
                return Respond(conn, 416, "text/plain", "", keep_alive, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
            }
            status = 206;
            extra += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size) + "\r\n";
        }
        const std::uint64_t length = size ? last - first + 1 : 0;
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\nContent-Type: " + MimeFor(file) +
                           "\r\nContent-Length: " + std::to_string(length) + "\r\n" + (keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n") +
                           extra + "\r\n";
        if (!conn.SendText(head)) return false;
        if (head_only || !length) return true;
        in.seekg(static_cast<std::streamoff>(first));
        std::vector<char> buffer(1 << 19);
        std::uint64_t left = length;
        while (left && running) {
            const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));
            in.read(buffer.data(), want);
            const auto got = in.gcount();
            if (got <= 0) return false;
            if (!conn.Send(buffer.data(), static_cast<std::size_t>(got))) return false;
            left -= static_cast<std::uint64_t>(got);
        }
        return left == 0;
    }

    // ---- server-sent events
    void ServeEvents(Connection& conn, const Request& request) {
        auto session = std::make_shared<Session>();
        session->token = request.token;
        session->user_id = request.user_id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            session->id = next_session++;
            sessions[session->id] = session;
            // Keep the number of idle sessions bounded (a client that reloads often).
            while (sessions.size() > 32) {
                sessions.begin()->second->alive = false;
                sessions.begin()->second->wake.notify_all();
                sessions.erase(sessions.begin());
            }
        }
        const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                                 "Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n";
        bool ok = conn.SendText(head) && conn.SendText("retry: 2000\n\ndata: {\"type\":\"session\",\"id\":" + std::to_string(session->id) + "}\n\n");
        std::unique_lock<std::mutex> lock(mutex);
        while (ok && running && session->alive) {
            if (session->queue.empty()) {
                session->wake.wait_for(lock, std::chrono::seconds(15));
                if (session->queue.empty()) {
                    lock.unlock();
                    ok = conn.SendText(": ping\n\n");
                    lock.lock();
                    continue;
                }
            }
            std::string message = std::move(session->queue.front());
            session->queue.pop_front();
            lock.unlock();
            std::string frame;
            std::stringstream lines(message);
            std::string line;
            while (std::getline(lines, line)) frame += "data: " + line + "\n";
            frame += "\n";
            ok = conn.SendText(frame);
            lock.lock();
        }
        session->alive = false;
        sessions.erase(session->id);
    }

    // ---- request dispatch
    bool Handle(Connection& conn, Request& request, bool keep_alive) {
        const std::string& path = request.path;
        if (request.method == "OPTIONS") return Respond(conn, 204, "", "", keep_alive, "Allow: GET, HEAD, POST, DELETE, OPTIONS\r\n");
        if (path == "/" || path == "/index.html") return Respond(conn, 302, "", "", keep_alive, "Location: /ui/index.html\r\n");
        if (path.rfind("/ui/", 0) == 0) {
            if (request.method != "GET" && request.method != "HEAD") return Error(conn, 405, keep_alive);
            const fs::path file = SafeJoin(hooks.ui_dir, path.substr(4));
            std::error_code ec;
            if (file.empty() || !fs::is_regular_file(file, ec)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "no-cache");
        }
        if (path == "/api/hello") {
            const bool authorized = Authorized(request);
            return Json(conn, 200,
                        "{\"name\":" + vp::Json::Quote(hooks.server_name) + ",\"id\":" + vp::Json::Quote(server_id) +
                            ",\"version\":1,\"authorized\":" + (authorized ? "true" : "false") + ",\"userId\":" + vp::Json::Quote(request.user_id) +
                            ",\"canDelete\":" + ((authorized && hooks.can_delete_media && hooks.can_delete_media(request.user_id)) ? "true" : "false") +
                            ",\"libraries\":" + (authorized && hooks.user_libraries ? hooks.user_libraries(request.user_id) : "[]") +
                            ",\"browserStreams\":" + (hooks.remux ? "true" : "false") +
                            ",\"streamMode\":" + vp::Json::Quote(authorized && hooks.stream_mode ? hooks.stream_mode() : "original") + "}",
                        keep_alive);
        }
        if (path == "/api/pair") {
            if (request.method != "POST") return Error(conn, 405, keep_alive);
            bool ok = false;
            const vp::Json body = vp::Json::Parse(request.body, &ok);
            const std::string device = ok ? body["device"].str("device").substr(0, 64) : "device";
            std::string token;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const std::int64_t now = NowMs();
                if (now < pin_locked_until) return Json(conn, 429, "{\"ok\":false,\"error\":\"locked\"}", keep_alive);
                if (!ok || body["pin"].str() != pin) {
                    if (++pin_failures >= 5) { pin_failures = 0; pin_locked_until = now + 60000; }
                    Log("LAN pairing rejected from " + request.peer);
                    return Json(conn, 403, "{\"ok\":false,\"error\":\"pin\"}", keep_alive);
                }
                pin_failures = 0;
                token = RandomHex(16);
                clients.push_back({token, device, request.peer, "default", now, now});
                SaveLocked();
            }
            Log("LAN client paired: " + device + " (" + request.peer + ")");
            return Json(conn, 200, "{\"ok\":true,\"token\":" + vp::Json::Quote(token) + "}", keep_alive,
                        "Set-Cookie: vp_token=" + token + "; Path=/; Max-Age=31536000; SameSite=Lax\r\n");
        }
        if (!Authorized(request)) return Error(conn, 401, keep_alive);

        if (path == "/api/config") {
            const std::string text = hooks.read_config ? hooks.read_config(request.user_id) : std::string("{}");
            return Json(conn, 200, text.empty() ? "{}" : text, keep_alive);
        }
        if (path == "/api/data") {
            const std::string key = request.Query("key");
            const bool safe = !key.empty() && key.size() <= 64 &&
                              std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
            if (!safe) return Error(conn, 400, keep_alive);
            if (request.method == "POST") {
                if (key != "progress" || !hooks.write_data || request.body.size() > 16 * 1024 * 1024)
                    return Error(conn, 403, keep_alive);
                const bool saved = hooks.write_data(request.user_id, key, request.body);
                return Json(conn, saved ? 200 : 400, saved ? "{\"ok\":true}" : "{\"ok\":false}", keep_alive);
            }
            if (request.method != "GET") return Error(conn, 405, keep_alive);
            const std::string text = hooks.read_data ? hooks.read_data(request.user_id, key) : std::string();
            return Json(conn, 200, "{\"key\":" + vp::Json::Quote(key) + ",\"text\":" + vp::Json::Quote(text) + "}", keep_alive);
        }
        if (path == "/api/events") {
            ServeEvents(conn, request);
            return false; // connection is finished
        }
        if (path == "/api/rpc") {
            if (request.method != "POST") return Error(conn, 405, keep_alive);
            std::uint64_t id = 0;
            try { id = std::stoull(request.Query("s", "0")); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = sessions.find(id);
                if (it == sessions.end() || (it->second->token != request.token && request.peer.rfind("127.", 0) != 0))
                    return Error(conn, 403, keep_alive);
            }
            if (request.body.size() > 256 * 1024) return Error(conn, 413, keep_alive);
            std::string user_id;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = sessions.find(id);
                if (it != sessions.end()) user_id = it->second->user_id;
            }
            if (hooks.rpc) hooks.rpc(id, user_id, request.body);
            return Respond(conn, 204, "", "", keep_alive);
        }
        if (path.rfind("/cache/", 0) == 0 || path.rfind("/thumbs/", 0) == 0) {
            const bool cache = path.rfind("/cache/", 0) == 0;
            const std::string relative = path.substr(cache ? 7 : 8);
            if (!cache && hooks.can_access_thumbnail && !hooks.can_access_thumbnail(request.user_id, relative)) return Error(conn, 404, keep_alive);
            const fs::path file = SafeJoin(hooks.data_dir / (cache ? "cache" : "thumbs"), relative);
            std::error_code ec;
            if (file.empty() || !fs::is_regular_file(file, ec)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "max-age=600");
        }
        if (path.rfind("/fonts/", 0) == 0) {
            const std::string name = path.substr(7);
            if (hooks.fonts_dir.empty() || name.find('/') != std::string::npos) return Error(conn, 404, keep_alive);
            const fs::path file = SafeJoin(hooks.fonts_dir, name);
            std::error_code ec;
            if (file.empty() || !IsFontExt(Extension(file)) || !fs::is_regular_file(file, ec)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "max-age=86400");
        }

        // Library files: the absolute UTF-8 path in ?p= must be inside a configured library folder.
        const fs::path file = PathFromUtf8(request.Query("p"));
        const auto library_file = [&](bool (*ext_ok)(const std::string&)) {
            std::error_code ec;
            return !file.empty() && file.is_absolute() && ext_ok(Extension(file)) && hooks.can_access_file && hooks.can_access_file(request.user_id, file) &&
                   fs::is_regular_file(file, ec);
        };
        if (path == "/api/media") {
            if (request.method != "DELETE") return Error(conn, 405, keep_alive);
            if (!library_file(&IsVideoExt) || !hooks.can_delete_media || !hooks.can_delete_media(request.user_id) || !hooks.delete_media)
                return Error(conn, 403, keep_alive);
            const bool deleted = hooks.delete_media(request.user_id, file);
            return Json(conn, deleted ? 200 : 500, deleted ? "{\"ok\":true}" : "{\"ok\":false}", keep_alive);
        }
        if (path == "/media/f") {
            if (!library_file(&IsMetaExt)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "max-age=600");
        }
        if (path == "/stream/file") {
            if (!library_file(&IsVideoExt)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "no-store");
        }
        if (path == "/stream/sub") {
            if (!library_file(&IsSubtitleExt)) return Error(conn, 404, keep_alive);
            return SendFile(conn, request, file, keep_alive, "no-store");
        }
        if (path == "/api/probe") {
            if (!library_file(&IsVideoExt) || !hooks.probe) return Error(conn, 404, keep_alive);
            const std::string json = hooks.probe(file);
            return json.empty() ? Error(conn, 500, keep_alive) : Json(conn, 200, json, keep_alive);
        }
        if (path == "/api/subtitle") {
            if (!library_file(&IsVideoExt) || !hooks.subtitle) return Error(conn, 404, keep_alive);
            int stream = -1;
            try { stream = std::stoi(request.Query("i", "-1")); } catch (...) {}
            const std::string text = hooks.subtitle(file, stream);
            if (text.empty()) return Error(conn, 404, keep_alive);
            return Respond(conn, 200, "text/plain; charset=utf-8", text, keep_alive, "Cache-Control: max-age=600\r\n");
        }
        if (path == "/api/seekpoint") {
            if (!library_file(&IsVideoExt) || !hooks.seekpoint) return Error(conn, 404, keep_alive);
            std::int64_t ms = 0;
            try { ms = std::stoll(request.Query("ms", "0")); } catch (...) {}
            const std::string json = hooks.seekpoint(file, ms);
            return json.empty() ? Error(conn, 500, keep_alive) : Json(conn, 200, json, keep_alive);
        }
        if (path == "/stream/remux") {
            if (!library_file(&IsVideoExt) || !hooks.remux) return Error(conn, 404, keep_alive);
            if (!hooks.remux(request, file, conn) && !conn.closed()) return Error(conn, 500, false);
            return false; // chunked stream: close afterwards
        }
        return Error(conn, 404, keep_alive);
    }

    // ---- connection loop
    void Serve(socket_t s, std::string peer) {
        ++active_connections;
        SetTimeouts(s, 30000);
        SocketConnection conn(s);
        std::string buffer;
        char chunk[16384];
        bool keep = true;
        while (keep && running) {
            // Read the header block.
            std::size_t header_end;
            while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
                if (buffer.size() > 32768) { keep = false; break; }
                const int n = static_cast<int>(::recv(s, chunk, sizeof chunk, 0));
                if (n <= 0) { keep = false; break; }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            if (!keep) break;
            Request request;
            request.peer = peer;
            std::stringstream head(buffer.substr(0, header_end));
            buffer.erase(0, header_end + 4);
            std::string line, target, version;
            std::getline(head, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            {
                std::stringstream first(line);
                first >> request.method >> target >> version;
            }
            const auto target_query = target.find('?');
            request.path = PercentDecode(target.substr(0, target_query));
            while (std::getline(head, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                request.headers[Lower(line.substr(0, colon))] = value;
            }
            std::size_t length = 0;
            if (auto it = request.headers.find("content-length"); it != request.headers.end()) {
                try { length = static_cast<std::size_t>(std::stoull(it->second)); } catch (...) { length = 0; }
            }
            const std::size_t max_body = request.method == "POST" && request.path == "/api/data" ? 16 * 1024 * 1024 : 1024 * 1024;
            if (length > max_body) { Impl::Error(conn, 413, false); break; }
            while (buffer.size() < length) {
                const int n = static_cast<int>(::recv(s, chunk, sizeof chunk, 0));
                if (n <= 0) { keep = false; break; }
                buffer.append(chunk, static_cast<std::size_t>(n));
            }
            if (!keep) break;
            request.body = buffer.substr(0, length);
            buffer.erase(0, length);

            if (target_query != std::string::npos) {
                std::stringstream pairs(target.substr(target_query + 1));
                std::string pair;
                while (std::getline(pairs, pair, '&')) {
                    const auto eq = pair.find('=');
                    request.query[PercentDecode(pair.substr(0, eq))] = eq == std::string::npos ? "" : PercentDecode(pair.substr(eq + 1));
                }
            }
            if (auto it = request.headers.find("x-vp-token"); it != request.headers.end()) request.token = it->second;
            if (request.token.empty()) request.token = request.Query("t");
            if (auto it = request.headers.find("cookie"); request.token.empty() && it != request.headers.end()) {
                const auto at = it->second.find("vp_token=");
                if (at != std::string::npos) request.token = it->second.substr(at + 9, 32);
            }
            const auto connection = request.headers.find("connection");
            const bool client_keep = version == "HTTP/1.1" ? !(connection != request.headers.end() && Lower(connection->second) == "close")
                                                           : (connection != request.headers.end() && Lower(connection->second) == "keep-alive");
            if (request.method.empty() || target.empty() || target[0] != '/') { Impl::Error(conn, 400, false); break; }
            keep = Handle(conn, request, client_keep) && client_keep && !conn.closed();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            open_sockets.erase(s);
        }
        ::shutdown(s, VP_SHUT_BOTH);
        VP_CLOSE(s);
        --active_connections;
    }

    void AcceptLoop() {
        while (running) {
            // Wait with a timeout instead of blocking in accept(): closing the listener from
            // another thread does not reliably wake a blocked accept() on Windows, which left
            // Stop() (and so WM_CLOSE) joining this thread forever.
#ifdef _WIN32
            WSAPOLLFD ready{listener, POLLRDNORM, 0};
            const int polled = ::WSAPoll(&ready, 1, 250);
#else
            pollfd ready{listener, POLLIN, 0};
            const int polled = ::poll(&ready, 1, 250);
#endif
            if (!running) break;
            if (polled <= 0) continue;
            sockaddr_in addr{};
#ifdef _WIN32
            int len = sizeof addr;
#else
            socklen_t len = sizeof addr;
#endif
            const socket_t s = ::accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
            if (s == VP_INVALID_SOCKET) {
                if (!running) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
            const std::string peer = text;
            if (!IsPrivateIPv4(peer) || active_connections.load() > 64) {
                VP_CLOSE(s);
                continue;
            }
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            {
                std::lock_guard<std::mutex> lock(mutex);
                open_sockets.insert(s);
            }
            std::thread(&Impl::Serve, this, s, peer).detach();
        }
    }

    void DiscoveryLoop() {
        char buffer[512];
        while (running) {
            sockaddr_in from{};
#ifdef _WIN32
            int len = sizeof from;
#else
            socklen_t len = sizeof from;
#endif
            const int n = static_cast<int>(::recvfrom(udp, buffer, sizeof buffer - 1, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n <= 0) continue; // timeout (1 s) lets Stop() end the loop
            buffer[n] = 0;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
            if (!IsPrivateIPv4(text) || std::strncmp(buffer, kDiscoveryProbe, std::strlen(kDiscoveryProbe)) != 0) continue;
            std::string reply;
            {
                std::lock_guard<std::mutex> lock(mutex);
                reply = "{\"vplan\":1,\"name\":" + vp::Json::Quote(hooks.server_name) + ",\"id\":" + vp::Json::Quote(server_id) +
                        ",\"port\":" + std::to_string(port) + "}";
            }
            ::sendto(udp, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), len);
        }
    }
};

// ---------------------------------------------------------------- Server
Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { Stop(); }

bool Server::Start(std::uint16_t port, LanHooks hooks, std::string* error) {
    Stop();
    EnsureSockets();
    auto& d = *impl_;
    d.hooks = std::move(hooks);
    d.Load();
    if (d.pin.empty()) d.pin = RandomPin();

    d.listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (d.listener == VP_INVALID_SOCKET) { if (error) *error = "socket() failed"; return false; }
#ifdef _WIN32
    BOOL exclusive = TRUE;
    setsockopt(d.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
#else
    int reuse = 1;
    setsockopt(d.listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(d.listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(d.listener, 16) != 0) {
        if (error) *error = "port " + std::to_string(port) + " is in use or blocked";
        VP_CLOSE(d.listener);
        d.listener = VP_INVALID_SOCKET;
        return false;
    }
    d.port = port;
    d.running = true;
    d.accept_thread = std::thread(&Impl::AcceptLoop, &d);

    d.udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d.udp != VP_INVALID_SOCKET) {
        int reuse = 1;
        setsockopt(d.udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
        sockaddr_in uaddr{};
        uaddr.sin_family = AF_INET;
        uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        uaddr.sin_port = htons(kDiscoveryPort);
        if (::bind(d.udp, reinterpret_cast<sockaddr*>(&uaddr), sizeof uaddr) == 0) {
            SetTimeouts(d.udp, 1000);
            d.udp_thread = std::thread(&Impl::DiscoveryLoop, &d);
        } else {
            d.Log("LAN discovery port " + std::to_string(kDiscoveryPort) + " unavailable; clients must enter the address");
            VP_CLOSE(d.udp);
            d.udp = VP_INVALID_SOCKET;
        }
    }
    d.Log("LAN server listening on port " + std::to_string(port));
    return true;
}

void Server::Stop() {
    auto& d = *impl_;
    if (!d.running.exchange(false)) return;
    // AcceptLoop polls with a 250 ms timeout and sees running == false on its own; close the
    // listener only after it has exited so no thread is still using the socket.
    if (d.accept_thread.joinable()) d.accept_thread.join();
    if (d.listener != VP_INVALID_SOCKET) {
        ::shutdown(d.listener, VP_SHUT_BOTH);
        VP_CLOSE(d.listener);
        d.listener = VP_INVALID_SOCKET;
    }
    if (d.udp_thread.joinable()) d.udp_thread.join();
    if (d.udp != VP_INVALID_SOCKET) { VP_CLOSE(d.udp); d.udp = VP_INVALID_SOCKET; }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        for (auto& [id, session] : d.sessions) { session->alive = false; session->wake.notify_all(); }
        for (const socket_t s : d.open_sockets) ::shutdown(s, VP_SHUT_BOTH);
    }
    for (int i = 0; i < 100 && d.active_connections.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool Server::running() const { return impl_->running.load(); }
std::uint16_t Server::port() const { return impl_->port; }

void Server::Push(std::uint64_t session, const std::string& json) {
    auto& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    auto it = d.sessions.find(session);
    if (it == d.sessions.end()) return;
    auto& queue = it->second->queue;
    if (queue.size() >= 4000) queue.pop_front();
    queue.push_back(json);
    it->second->wake.notify_all();
}

bool Server::HasSession(std::uint64_t session) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sessions.count(session) != 0;
}

std::string Server::Pin() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pin;
}

void Server::RegeneratePin() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pin = RandomPin();
    impl_->pin_failures = 0;
}

std::string Server::StatusJson() const {
    auto& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    std::string out = "{\"running\":" + std::string(d.running ? "true" : "false") + ",\"port\":" + std::to_string(d.port) +
                      ",\"pin\":" + vp::Json::Quote(d.pin) + ",\"name\":" + vp::Json::Quote(d.hooks.server_name) + ",\"addresses\":[";
    const auto addresses = LocalAddresses();
    for (std::size_t i = 0; i < addresses.size(); ++i) out += std::string(i ? "," : "") + vp::Json::Quote(addresses[i]);
    out += "],\"clients\":[";
    for (std::size_t i = 0; i < d.clients.size(); ++i) {
        const auto& c = d.clients[i];
        out += std::string(i ? "," : "") + "{\"id\":" + vp::Json::Quote(c.token.substr(0, 8)) + ",\"device\":" + vp::Json::Quote(c.device) +
               ",\"peer\":" + vp::Json::Quote(c.peer) + ",\"userId\":" + vp::Json::Quote(c.user_id) + ",\"created\":" + std::to_string(c.created) + ",\"last\":" + std::to_string(c.last) + "}";
    }
    return out + "]}";
}

bool Server::Revoke(const std::string& token_prefix) {
    auto& d = *impl_;
    if (token_prefix.size() < 8) return false;
    std::lock_guard<std::mutex> lock(d.mutex);
    const auto before = d.clients.size();
    d.clients.erase(std::remove_if(d.clients.begin(), d.clients.end(), [&](const ClientRecord& c) { return c.token.rfind(token_prefix, 0) == 0; }),
                    d.clients.end());
    if (d.clients.size() == before) return false;
    for (auto& [id, session] : d.sessions)
        if (session->token.rfind(token_prefix, 0) == 0) { session->alive = false; session->wake.notify_all(); }
    d.SaveLocked();
    return true;
}

bool Server::AssignUser(const std::string& token_prefix, const std::string& user_id) {
    auto& d = *impl_;
    if (token_prefix.size() < 8 || user_id.empty()) return false;
    std::lock_guard<std::mutex> lock(d.mutex);
    auto it = std::find_if(d.clients.begin(), d.clients.end(), [&](const ClientRecord& c) { return c.token.rfind(token_prefix, 0) == 0; });
    if (it == d.clients.end()) return false;
    if (it->user_id == user_id) return true;
    it->user_id = user_id;
    for (auto& [id, session] : d.sessions)
        if (session->token == it->token) { session->alive = false; session->wake.notify_all(); }
    d.SaveLocked();
    return true;
}

void Server::RefreshSessions() {
    auto& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    for (auto& [id, session] : d.sessions) { session->alive = false; session->wake.notify_all(); }
}

// ---------------------------------------------------------------- discovery (client)
std::vector<DiscoveredServer> Discover(int timeout_ms) {
    EnsureSockets();
    std::vector<DiscoveredServer> found;
    const socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == VP_INVALID_SOCKET) return found;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    SetTimeouts(s, 200);
    std::vector<std::string> targets = {"255.255.255.255", "127.0.0.1"};
    for (const auto& a : LocalAddresses()) targets.push_back(a.substr(0, a.rfind('.')) + ".255"); // assume /24 home networks
    for (const auto& t : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        inet_pton(AF_INET, t.c_str(), &to.sin_addr);
        ::sendto(s, kDiscoveryProbe, static_cast<int>(std::strlen(kDiscoveryProbe)), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buffer[1024];
    while (std::chrono::steady_clock::now() < end) {
        sockaddr_in from{};
#ifdef _WIN32
        int len = sizeof from;
#else
        socklen_t len = sizeof from;
#endif
        const int n = static_cast<int>(::recvfrom(s, buffer, sizeof buffer - 1, 0, reinterpret_cast<sockaddr*>(&from), &len));
        if (n <= 0) continue;
        buffer[n] = 0;
        bool ok = false;
        const vp::Json json = vp::Json::Parse(buffer, &ok);
        if (!ok || json["vplan"].i64() != 1) continue;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
        DiscoveredServer server{json["name"].str(), json["id"].str(), "http://" + std::string(text) + ":" + std::to_string(json["port"].i64())};
        // The same server answers on several interfaces (and on loopback): keep one, preferring a LAN address.
        auto it = std::find_if(found.begin(), found.end(), [&](const DiscoveredServer& f) { return f.id == server.id; });
        if (it == found.end()) found.push_back(server);
        else if (it->address.find("//127.") != std::string::npos) *it = server;
    }
    VP_CLOSE(s);
    return found;
}

} // namespace vp::lan
