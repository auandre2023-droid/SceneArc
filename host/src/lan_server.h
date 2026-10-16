// LAN sharing: a small HTTP/1.1 server (plus UDP discovery) that lets assigned users on the
// local network browse permitted libraries, stream originals, and use scoped playback progress.
//
// The server is self-contained and portable (Winsock on Windows, BSD sockets elsewhere, so the
// request handling can be tested off Windows). Everything host-specific - which files belong to
// the library, how data is sanitised, how RPC commands run, how a browser remux is produced - is
// supplied through LanHooks by main.cpp.
//
// Routes (all GET unless noted):
//   /                       -> 302 /ui/index.html
//   /ui/...                 static UI (no auth: the page itself shows the PIN prompt)
//   /api/hello              {name, id, version, authorized}
//   POST /api/pair          {pin, device} -> {ok, token} and cookie vp_token
//   /api/config             sanitised config.json (hooks.read_config)
//   /api/data?key=          sanitised data\<key>.json (hooks.read_data)
//   POST /api/data?key=progress  save this assigned user's progress
//   DELETE /api/media?p=         move an authorised original video to the Recycle Bin
//   /api/events             Server-Sent Events; first event {type:"session", id}
//   POST /api/rpc?s=<id>    one UI command {cmd, reqId?, ...}; replies arrive on the session's events
//   /cache/..., /thumbs/... files under data\cache, data\thumbs
//   /fonts/<file>           Windows fonts (font extensions only)
//   /media/f?p=<utf8 path>  .nfo / artwork inside the library
//   /stream/file?p=         original video file (byte ranges)
//   /stream/sub?p=          side-car subtitle file
//   /stream/remux?p=&start=&audio=   browser stream (hooks.remux)
//   /api/probe?p=           stream list for the browser player (hooks.probe)
//   /api/subtitle?p=&i=     embedded text subtitle as ASS (hooks.subtitle)
//   /api/seekpoint?p=&ms=   where /stream/remux?start=ms really starts (the key frame before ms)
// Every route except /ui, /api/hello and /api/pair needs a paired token (cookie vp_token, header
// X-VP-Token or query t=). Only private IPv4 peers are served.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vp::lan {

constexpr std::uint16_t kDefaultHttpPort = 47880;
constexpr std::uint16_t kDiscoveryPort = 47881;
constexpr const char* kDiscoveryProbe = "VPLAN_DISCOVER 1";

std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);
std::string PercentDecode(const std::string& text);
std::string PercentEncode(const std::string& text);

struct Request {
    std::string method;
    std::string path;                          // decoded, without query
    std::map<std::string, std::string> query;  // decoded values
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
    std::string peer;                          // dotted IPv4
    std::string token;                         // from cookie / header / query
    std::string user_id;                       // resolved from the paired device
    std::string Query(const std::string& key, const std::string& fallback = {}) const {
        auto it = query.find(key);
        return it == query.end() ? fallback : it->second;
    }
};

// Writes one HTTP response on a connection. Streams may use chunked transfer for bodies of
// unknown length (remux) or Content-Length for files.
class Connection {
public:
    virtual ~Connection() = default;
    virtual bool Send(const void* data, std::size_t size) = 0;
    bool SendText(const std::string& text) { return Send(text.data(), text.size()); }
    // Starts a chunked response with the given status, content type and extra header lines.
    bool BeginChunked(int status, const std::string& content_type, const std::string& extra_headers = {});
    bool SendChunk(const void* data, std::size_t size);
    bool EndChunked();
    bool closed() const { return closed_; }
protected:
    bool closed_ = false;
};

struct LanHooks {
    std::filesystem::path ui_dir;     // ...\ui
    std::filesystem::path data_dir;   // ...\data
    std::filesystem::path fonts_dir;  // %WINDIR%\Fonts (may be empty)
    std::string server_name;
    std::function<std::string()> stream_mode; // original or superres; advertised only to paired clients
    std::function<bool(const std::string& user_id, const std::filesystem::path&)> can_access_file;
    std::function<bool(const std::string& user_id, const std::string& relative_path)> can_access_thumbnail;
    std::function<bool(const std::string& user_id)> can_delete_media;
    std::function<std::string(const std::string& user_id)> user_libraries; // JSON array of permitted library IDs
    std::function<std::string(const std::string& user_id)> read_config; // sanitised JSON text
    std::function<std::string(const std::string& user_id, const std::string& key)> read_data; // sanitised JSON text ("" = none)
    std::function<bool(const std::string& user_id, const std::string& key, const std::string& text)> write_data;
    std::function<void(std::uint64_t session, const std::string& user_id, const std::string& json)> rpc;
    std::function<bool(const std::string& user_id, const std::filesystem::path& file)> delete_media;
    // Optional browser support (remux_stream.cpp). Return false when the request was not handled.
    std::function<bool(const Request&, const std::filesystem::path& file, Connection&)> remux;
    std::function<std::string(const std::filesystem::path& file)> probe;      // JSON ("" = error)
    std::function<std::string(const std::filesystem::path& file, int stream)> subtitle; // ASS text
    std::function<std::string(const std::filesystem::path& file, std::int64_t ms)> seekpoint; // {"startMs":n}
    std::function<void(const std::string& message)> log;
};

struct DiscoveredServer {
    std::string name;
    std::string id;
    std::string address; // http://a.b.c.d:port
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool Start(std::uint16_t port, LanHooks hooks, std::string* error);
    void Stop();
    bool running() const;
    std::uint16_t port() const;

    // Sends one JSON message to a browser/client session (SSE). Thread-safe.
    void Push(std::uint64_t session, const std::string& json);
    bool HasSession(std::uint64_t session) const;

    std::string Pin() const;
    void RegeneratePin();
    std::string StatusJson() const;   // {running, port, pin, addresses:[...], clients:[...]}
    bool Revoke(const std::string& token_prefix);
    bool AssignUser(const std::string& token_prefix, const std::string& user_id);
    void RefreshSessions();

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Client side: broadcasts a probe and collects replies for timeout_ms.
std::vector<DiscoveredServer> Discover(int timeout_ms);

// True for 10/8, 172.16/12, 192.168/16, 169.254/16 and 127/8.
bool IsPrivateIPv4(const std::string& dotted);

} // namespace vp::lan
