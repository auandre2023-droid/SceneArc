// Video Player host: one Win32 window, a DirectComposition tree with the
// PlaybackCore video swap chain at the bottom and a transparent WebView2
// (visual hosting) on top, so the web UI's controls float over the picture.
//
//   root visual
//    |- video visual   (content = playbackcore.dll composition swap chain)
//    `- webview visual (ICoreWebView2CompositionController::RootVisualTarget)
//
// The web UI (ui/index.html, served as https://app.local/) talks to this host with
// chrome.webview.postMessage({cmd: ...}); the host answers / forwards player events
// with PostWebMessageAsJson({type: ...}). See ui/app.js for the message list.
//
// The host also starts PlaybackCore.AudioHost (ASIO by default, optionally WASAPI) as a hidden child
// process tied to a job object, so it always exits together with the player.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>

#include "WebView2.h"
#if defined(VP_ENGINE_MPV)
#include "mpv_backend.h"
#include "mpv_config_safety.h"
#include "media_tools.h"
#include <dbghelp.h>
#include <bcrypt.h>
#include <wbemidl.h>
#pragma comment(lib, "bcrypt.lib")   // diagnostics export: SHA-256
#pragma comment(lib, "wbemuuid.lib") // diagnostics export: Win32_VideoController.DriverVersion
#else
#include "playbackcore_api.h"
#endif
#include "json.h"
#include "lan_server.h"
#ifndef VP_NO_REMUX
#include "remux_stream.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
#include <mutex>
#include <memory>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr UINT WM_APP_PLAYER = WM_APP + 1;   // wParam = event, lParam = std::string* (json)
constexpr UINT WM_APP_POST = WM_APP + 2;     // lParam = std::string* (utf-8 json to post to the web UI)
constexpr UINT WM_APP_LAN = WM_APP + 3;      // lParam = LanMessage* (LAN client command, or a downloaded subtitle)
constexpr UINT WM_APP_MPV_WAKE = WM_APP + 4;
constexpr UINT WM_APP_WARM_PLAYER = WM_APP + 5; // load libmpv once the UI page has loaded
constexpr UINT WM_APP_RUN = WM_APP + 6;         // lParam = std::function<void()>* to run on the UI thread
constexpr wchar_t kClassName[] = L"VideoPlayerHost";
constexpr wchar_t kMpvChildClass[] = L"VideoPlayerMpvSurface";

// ---------------------------------------------------------------- utilities
std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::wstring QuoteProcessArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') { ++backslashes; continue; }
        if (ch == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += L'\"';
        } else {
            quoted.append(backslashes, L'\\');
            quoted += ch;
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'\"';
    return quoted;
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string ReadFileUtf8(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    return text;
}

// ReadFileUtf8 plus why a file gave no text: "ok" (read; the text may still be empty), "missing" (no such
// file: a first run) or "error" (the file is there but could not be opened or read). The page must never
// treat "error" like "missing", or its first save would replace a data file it could not read.
std::string ReadFileUtf8Status(const fs::path& path, const char** state) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found) { *state = "missing"; return {}; }
    if (ec || status.type() != fs::file_type::regular) { *state = "error"; return {}; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { *state = "error"; return {}; }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) { *state = "error"; return {}; }
    std::string text = buffer.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    *state = "ok";
    return text;
}

// 1B-b H1: <path>.tmp written in full and flushed to the disk (FlushFileBuffers) before the rename, so after a power
// loss the renamed file holds the new bytes, not a zero-length or partly written file. Measured cost: see the 1B-b
// host report (a few ms for the 3.5 MB anilist.json on an SSD).
bool WriteTempFileDurable(const fs::path& temp, const std::string& text) {
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - done, 16u * 1024 * 1024));
        DWORD written = 0;
        ok = WriteFile(file, text.data() + done, chunk, &written, nullptr) != FALSE && written == chunk;
        done += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

bool WriteFileUtf8(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp";
    if (!WriteTempFileDurable(temp, text)) return false;
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// 1B-b H5: like WriteFileUtf8, and when `rotate` is set and <path> already exists, its previous content is kept as
// <path>.bak1 (.bak1 -> .bak2 -> .bak3 first; the old .bak3 is replaced). No data is copied: the new text is written
// and flushed to <path>.tmp first, then .bak1 becomes a second hard link to the current file and the rename of
// .tmp over <path> leaves the old content under .bak1 only, so <path> itself is never missing. Where hard links are
// unavailable (FAT/exFAT), <path> is renamed to .bak1 right before .tmp is renamed into place (and renamed back if
// that fails). *rotated: whether .bak1 now holds the previous content.
bool WriteFileUtf8WithBackup(const fs::path& path, const std::string& text, bool rotate, bool* rotated) {
    if (rotated) *rotated = false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp";
    if (!WriteTempFileDurable(temp, text)) return false;
    bool moved_away = false;
    const DWORD exists = GetFileAttributesW(path.c_str());
    if (rotate && exists != INVALID_FILE_ATTRIBUTES && !(exists & FILE_ATTRIBUTE_DIRECTORY)) {
        const auto bak = [&](int n) { return fs::path(path.wstring() + L".bak" + std::to_wstring(n)); };
        constexpr DWORD kMove = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
        if (GetFileAttributesW(bak(2).c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(bak(2).c_str(), bak(3).c_str(), kMove);
        if (GetFileAttributesW(bak(1).c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(bak(1).c_str(), bak(2).c_str(), kMove);
        if (GetFileAttributesW(bak(1).c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (CreateHardLinkW(bak(1).c_str(), path.c_str(), nullptr)) {
                if (rotated) *rotated = true;
            } else if (MoveFileExW(path.c_str(), bak(1).c_str(), kMove)) {
                moved_away = true;
                if (rotated) *rotated = true;
            }
        }
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    if (moved_away) {
        MoveFileExW((path.wstring() + L".bak1").c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH); // put the old file back
        if (rotated) *rotated = false;
    }
    return false;
}

// 1B-b H2: whether `text` is one complete JSON value whose top level is an object (RFC 8259 grammar, what the page's
// JSON.parse accepts). A validating scan only: nothing is allocated, so checking the 3.5 MB anilist store costs a few
// ms on the UI thread, unlike building a vp::Json tree. Nesting deeper than 512 levels is refused (stack bound).
// *why (optional): where the first problem is, for the log.
class JsonObjectValidator {
public:
    explicit JsonObjectValidator(const std::string& text) : s_(text) {}
    bool Run(std::string* why) {
        Space();
        bool ok = pos_ < s_.size() && s_[pos_] == '{' && Value(0);
        if (ok) { Space(); ok = pos_ == s_.size(); }
        if (!ok && why) *why = s_.empty() ? std::string("empty") : pos_ >= s_.size() ? std::string("truncated") : "byte " + std::to_string(pos_);
        return ok;
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;
    void Space() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_; }
    bool Lit(const char* word) {
        for (; *word; ++word, ++pos_) if (pos_ >= s_.size() || s_[pos_] != *word) return false;
        return true;
    }
    static bool Hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    bool String() {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            if (e == 'u') {
                for (int i = 0; i < 4; ++i, ++pos_) if (pos_ >= s_.size() || !Hex(s_[pos_])) return false;
            } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' && e != 'r' && e != 't') {
                return false;
            }
        }
        return false;
    }
    bool Digits() {
        const std::size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        return pos_ > start;
    }
    bool Number() {
        if (s_[pos_] == '-') ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '0') ++pos_;
        else if (!Digits()) return false;
        if (pos_ < s_.size() && s_[pos_] == '.') { ++pos_; if (!Digits()) return false; }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!Digits()) return false;
        }
        return true;
    }
    bool Value(int depth) {
        Space();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{' || c == '[') {
            if (depth >= 512) return false;
            const char close = c == '{' ? '}' : ']';
            ++pos_;
            Space();
            if (pos_ < s_.size() && s_[pos_] == close) { ++pos_; return true; }
            for (;;) {
                if (c == '{') {
                    Space();
                    if (pos_ >= s_.size() || s_[pos_] != '"' || !String()) return false;
                    Space();
                    if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                    ++pos_;
                }
                if (!Value(depth + 1)) return false;
                Space();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == close) { ++pos_; return true; }
                return false;
            }
        }
        if (c == '"') return String();
        if (c == 't') return Lit("true");
        if (c == 'f') return Lit("false");
        if (c == 'n') return Lit("null");
        if (c == '-' || (c >= '0' && c <= '9')) return Number();
        return false;
    }
};

bool IsJsonObjectText(const std::string& text, std::string* why = nullptr) { return JsonObjectValidator(text).Run(why); }

// Data keys from the web UI become file names: keep them to [A-Za-z0-9_-].
bool SafeKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    return std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

fs::path ExeDir() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, n)).parent_path();
}

std::uint64_t AdapterLuidValue(const LUID& luid) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) |
           static_cast<std::uint32_t>(luid.LowPart);
}

std::string EnumerateGpuAdaptersJson() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "[]";
    std::string json = "[";
    bool first = true;
    std::uint32_t hardware_index = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumerated)) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            if (!first) json += ",";
            first = false;
            json += "{\"index\":" + std::to_string(hardware_index++) +
                    ",\"luid\":" + vp::Json::Quote(std::to_string(AdapterLuidValue(desc.AdapterLuid))) +
                    ",\"name\":" + vp::Json::Quote(Utf8(desc.Description)) +
                    ",\"vendorId\":" + std::to_string(desc.VendorId) +
                    ",\"deviceId\":" + std::to_string(desc.DeviceId) +
                    ",\"dedicatedVideoMemory\":" + std::to_string(static_cast<std::uint64_t>(desc.DedicatedVideoMemory)) + "}";
        }
        adapter.Reset();
    }
    return json + "]";
}

bool ParseGpuAdapterLuid(const std::string& text, std::uint64_t* luid) {
    if (!luid) return false;
    *luid = 0;
    if (text.empty() || text == "auto") return true;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || !end || *end != '\0') return false;
    *luid = static_cast<std::uint64_t>(value);
    return *luid != 0;
}

// The app root is the first ancestor of the exe (or the exe dir) holding ui/index.html.
fs::path FindAppRoot() {
    fs::path dir = ExeDir();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (fs::exists(dir / L"ui" / L"index.html")) return dir;
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return ExeDir();
}

#if defined(VP_ENGINE_MPV)
// Open mode lists: mpv-lazy VapourSynth scripts in mpv\vs (the internal UAI template excluded) and
// every .glsl under mpv\shaders as forward-slash paths relative to that folder.
std::string EnhancementVsScriptsJson() {
    const fs::path dir = ExeDir() / L"mpv" / L"vs";
    std::error_code ec;
    std::vector<std::string> names;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            std::wstring extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            const std::string name = Utf8(entry.path().filename().wstring());
            if (entry.is_regular_file(ec) && extension == L".vpy" && name != "uai_template.vpy") names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    std::string json = "[";
    for (std::size_t i = 0; i < names.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(names[i]);
    return json + "]";
}

std::string EnhancementShaderLibraryJson() {
    const fs::path root = ExeDir() / L"mpv" / L"shaders";
    std::error_code ec;
    std::vector<std::string> files;
    if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::wstring extension = it->path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension != L".glsl") continue;
            std::wstring relative = it->path().lexically_relative(root).wstring();
            std::replace(relative.begin(), relative.end(), L'\\', L'/');
            files.push_back(Utf8(relative));
        }
    }
    std::sort(files.begin(), files.end());
    std::string json = "[";
    for (std::size_t i = 0; i < files.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(files[i]);
    return json + "]";
}

std::string EnhancementAiModelsJson() {
    const fs::path models_dir = ExeDir() / L"vs-runtime" / L"vs-plugins" / L"models";
    std::error_code ec;
    if (!fs::is_directory(models_dir, ec)) return "[]";
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(models_dir, ec)) {
        if (ec) break;
        std::wstring extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (!entry.is_regular_file(ec) || ec || extension != L".onnx") continue;
        const std::string name = Utf8(entry.path().filename().wstring());
        if (name.empty() || name == "." || name == "..") continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string json = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) json += ',';
        json += vp::Json::Quote(names[i]);
    }
    return json + "]";
}

std::string EnhancementRifeModelsJson() {
    const fs::path models_dir = ExeDir() / L"vs-runtime" / L"vs-plugins" / L"models";
    auto list = [&](const wchar_t* folder) {
        std::vector<std::string> names;
        std::error_code ec;
        const fs::path dir = models_dir / folder;
        if (fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                std::wstring extension = entry.path().extension().wstring();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (entry.is_regular_file(ec) && extension == L".onnx") names.push_back(Utf8(entry.path().filename().wstring()));
            }
        }
        std::sort(names.begin(), names.end());
        std::string json = "[";
        for (std::size_t i = 0; i < names.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(names[i]);
        return json + "]";
    };
    return "{\"v2\":" + list(L"rife_v2") + ",\"quality\":" + list(L"rife") + "}";
}

std::string EnhancementDrbaModelsJson() {
    const fs::path dir = ExeDir() / L"vs-runtime" / L"vs-plugins" / L"models" / L"drba";
    std::error_code ec;
    std::vector<std::string> names;
    if (fs::is_directory(dir, ec)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            std::wstring extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (entry.is_regular_file(ec) && extension == L".onnx") names.push_back(Utf8(entry.path().filename().wstring()));
        }
    }
    std::sort(names.begin(), names.end());
    std::string json = "[";
    for (std::size_t i = 0; i < names.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(names[i]);
    return json + "]";
}

std::string EnhancementModelCatalogText() {
    return ReadFileUtf8(ExeDir() / L"mpv" / L"models.json");
}
#endif

// ---------------------------------------------------------------- state
struct App {
    HWND window{};
    fs::path root;
#if defined(VP_ENGINE_MPV)
    HWND video_child{};
    std::unique_ptr<vp::MpvBackend> player;
    std::string player_error;
    std::string player_error_code;
#else
    pc_player* player{};
#endif

    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root_visual;
    ComPtr<IDCompositionVisual> video_visual;
    ComPtr<IDCompositionVisual> web_visual;

    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2CompositionController> composition;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool tracking_mouse{};
    bool fullscreen{};
    bool video_shown{}; // PlaybackCore: swap chain attached to the video visual
    bool closing{};     // core-reliability 2A: WM_CLOSE ran; late WebView2 creation callbacks must not resurrect state
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};

    HANDLE audio_job{};
    PROCESS_INFORMATION audio_process{};
    std::string audio_status = "stopped";
} g;

void PostToWeb(const std::string& json) {
    if (g.webview) g.webview->PostWebMessageAsJson(Wide(json).c_str());
}

// Core-reliability 2A (report 25-C §1.4): the WebView2 objects were created on this UI thread's STA and must be closed and
// released here, before CoUninitialize, not by the static destructor of `g` after it. Idempotent; every later use
// null-checks (PostToWeb, OnResize, ForwardMouse, ...), and after Close() WebView2 invokes no more handlers.
void ReleaseWebView() {
    if (g.controller) g.controller->Close();
    g.webview.Reset();
    g.controller.Reset();
    g.composition.Reset();
    g.environment.Reset();
}

// DirectComposition objects of the main window (target bound to g.window): released while the window still exists.
void ReleaseComposition() {
    g.web_visual.Reset();
    g.video_visual.Reset();
    g.root_visual.Reset();
    g.target.Reset();
    g.dcomp.Reset();
}

// Thread-safe: worker threads hand their reply to the UI thread.
void PostToWebFromAnyThread(std::string json) {
    auto* payload = new std::string(std::move(json));
    if (!PostMessageW(g.window, WM_APP_POST, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

std::string Reply(const std::string& type, const vp::Json& request, const std::string& fields) {
    std::string json = "{\"type\":" + vp::Json::Quote(type);
    if (request["reqId"].type() == vp::Json::Type::Number) json += ",\"reqId\":" + std::to_string(request["reqId"].i64());
    if (!fields.empty()) json += "," + fields;
    return json + "}";
}

// ---------------------------------------------------------------- LAN sharing: reply routing
// Requests that arrive from a LAN client carry "__lan":<session>; their replies go to that
// client's event stream instead of the local WebView. Host-local URLs become server paths.
vp::lan::Server g_lan;
std::string g_lan_mode = "standalone";   // standalone | server | client
std::string g_lan_server;                // client mode: http://a.b.c.d:port

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

std::string LanRewrite(std::string json) {
    json = ReplaceAll(std::move(json), "https://thumbs.local/", "/thumbs/");
    json = ReplaceAll(std::move(json), "https://cache.local/", "/cache/");
    json = ReplaceAll(std::move(json), "https://media.local/f?p=", "/media/f?p=");
    return ReplaceAll(std::move(json), "https://fonts.local/", "/fonts/");
}

std::uint64_t LanSession(const vp::Json& request) {
    return request["__lan"].type() == vp::Json::Type::Number ? static_cast<std::uint64_t>(request["__lan"].i64()) : 0;
}

// Thread-safe: a reply for whoever sent `request`.
void ReplyTo(const vp::Json& request, std::string json) {
    if (const std::uint64_t session = LanSession(request)) g_lan.Push(session, LanRewrite(std::move(json)));
    else PostToWebFromAnyThread(std::move(json));
}

struct LanMessage {
    int kind = 0;            // 0: RPC from a LAN client, 1: downloaded subtitle file to load
    std::uint64_t session = 0;
    std::string text;
    std::string user_id;
};

// ---------------------------------------------------------------- config & audio host
fs::path ConfigPath() { return g.root / L"config.json"; }
fs::path DataDir() {
#if defined(VP_ENGINE_MPV)
    return ExeDir() / L"data";
#else
    return g.root / L"data";
#endif
}

#if defined(VP_ENGINE_MPV)
volatile LONG g_crash_dump_started = 0;
std::wstring g_crash_dump_dir;
std::wstring g_crash_host_log;

// Core-reliability 2A: one mpv-host.log line from the UI thread for exit-time paths where the backend's buffered log
// writer is not running (before the player exists, or after Shutdown stopped it). Rare paths only.
void AppendHostLogDirect(const std::string& text) {
    if (g_crash_host_log.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[32]{};
    std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond, now.wMilliseconds);
    const std::string line = stamp + text + "\r\n";
    const HANDLE log = CreateFileW(g_crash_host_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(log);
}

#if defined(VP_FAULT_INJECTION)
bool FaultEnv(const wchar_t* name) { wchar_t value[8]{}; return GetEnvironmentVariableW(name, value, 8) > 0; } // test builds only
#endif

bool IsFatalCrashCode(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == 0xc0000374 || code == 0xc0000409;
}

void WriteCrashDump(EXCEPTION_POINTERS* exception) {
    if (InterlockedCompareExchange(&g_crash_dump_started, 1, 0) != 0) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char marker[256]{};
    const DWORD code = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0;
    const int marker_size = std::snprintf(marker, sizeof(marker),
        "crash-tripwire code=0x%08lX pid=%lu tid=%lu time=%04u-%02u-%02uT%02u:%02u:%02u.%03u\r\n",
        static_cast<unsigned long>(code), static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

    HANDLE log = CreateFileW(g_crash_host_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        if (marker_size > 0 && static_cast<std::size_t>(marker_size) < sizeof(marker)) {
            DWORD written = 0;
            WriteFile(log, marker, static_cast<DWORD>(marker_size), &written, nullptr);
            FlushFileBuffers(log);
        }
        CloseHandle(log);
    }

    wchar_t dump_path[32768]{};
    swprintf_s(dump_path, std::size(dump_path),
        L"%ls\\mpv-crash-%04u%02u%02u-%02u%02u%02u-%03u-pid%lu.dmp",
        g_crash_dump_dir.c_str(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds, static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE dump = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    exception_info.ThreadId = GetCurrentThreadId();
    exception_info.ExceptionPointers = exception;
    exception_info.ClientPointers = FALSE;
    const MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, dump_type,
        exception ? &exception_info : nullptr, nullptr, nullptr);
    FlushFileBuffers(dump);
    CloseHandle(dump);
}

// Address ranges of the modules whose first-chance faults are dumped: VideoPlayer.exe (InstallCrashTripwire) and
// libmpv-2.dll (RecordCrashModule after the player loaded it). Plain values, so the handler needs no loader lock.
std::atomic<std::uintptr_t> g_crash_exe_begin{}, g_crash_exe_end{}, g_crash_mpv_begin{}, g_crash_mpv_end{};

bool ModuleRange(HMODULE module, std::uintptr_t* begin, std::uintptr_t* end) {
    if (!module) return false;
    const auto base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *begin = reinterpret_cast<std::uintptr_t>(base);
    *end = *begin + nt->OptionalHeader.SizeOfImage;
    return true;
}

void RecordCrashModule(HMODULE module, bool is_mpv) {
    std::uintptr_t begin = 0, end = 0;
    if (!ModuleRange(module, &begin, &end)) return;
    (is_mpv ? g_crash_mpv_end : g_crash_exe_end).store(end);
    (is_mpv ? g_crash_mpv_begin : g_crash_exe_begin).store(begin);
}

bool FaultInHostModule(const EXCEPTION_RECORD* record) {
    const auto pc = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    return (pc >= g_crash_exe_begin.load() && pc < g_crash_exe_end.load()) ||
           (pc >= g_crash_mpv_begin.load() && pc < g_crash_mpv_end.load());
}

// First-chance faults are dumped only when they are raised in VideoPlayer.exe or libmpv-2.dll. Drivers and runtimes
// (D3D/DirectML, CUDA/TensorRT, Python) raise and handle access violations of their own; writing a minidump from
// inside the process for those risked a loader-lock hang and left a crash-tripwire marker without a crash (audit
// C9 / row 12). An unhandled fault in any module is still dumped by CrashUnhandledHandler.
LONG WINAPI CrashVectoredHandler(PEXCEPTION_POINTERS exception) {
    if (exception && exception->ExceptionRecord && IsFatalCrashCode(exception->ExceptionRecord->ExceptionCode) &&
        FaultInHostModule(exception->ExceptionRecord))
        WriteCrashDump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI CrashUnhandledHandler(PEXCEPTION_POINTERS exception) {
    if (exception && exception->ExceptionRecord && IsFatalCrashCode(exception->ExceptionRecord->ExceptionCode))
        WriteCrashDump(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashTripwire() {
    const fs::path crash_dir = DataDir() / L"logs" / L"crash";
    const fs::path host_log = DataDir() / L"logs" / L"mpv-host.log";
    std::error_code ec;
    fs::create_directories(crash_dir, ec);
    if (ec) return;
    g_crash_dump_dir = crash_dir.wstring();
    g_crash_host_log = host_log.wstring();
    RecordCrashModule(GetModuleHandleW(nullptr), false);
    AddVectoredExceptionHandler(1, CrashVectoredHandler);
    SetUnhandledExceptionFilter(CrashUnhandledHandler);
}
#else
void AppendHostLogDirect(const std::string&) {} // PlaybackCore: no mpv-host.log
#endif

std::string DefaultConfig() {
    return "{\n"
           "  \"asioDriver\": \"\",\n"
           "  \"audioBackend\": \"asio\",\n"
           "  \"wasapiDeviceId\": \"\",\n"
           "  \"wasapiExclusive\": false,\n"
           "  \"audioHostExe\": \"\",\n"
           "  \"gpuAdapterLuid\": \"\",\n"
           "  \"allowSoftwareDecodeFallback\": false,\n"
           "  \"frameInterpolation\": false,\n"
           "  \"frameInterpolationBackend\": \"amf\",\n"
           "  \"rifeModel\": \"v46\",\n"
           "  \"rifeOutputWarp\": false,\n"
           "  \"frcDedup\": false,\n"
           "  \"restore\": {\"enabled\": false, \"model\": \"Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx\"},\n"
           "  \"vrr\": {\"enabled\": false, \"minHz\": 0, \"maxHz\": 0},\n"
           "  \"upscale\": {\"mode\": \"off\"},\n"
#if defined(VP_ENGINE_MPV)
           "  \"video\": {\"gpuApi\":\"d3d11\",\"gpuAdapterLuid\":\"\",\"hwdec\":\"d3d11va\",\"preset\":\"custom\",\"restore\":\"off\",\"upscaler\":\"off\",\"sharpen\":\"off\",\"sharpenStrength\":1.0,\"deband\":\"off\",\"smooth\":\"off\",\"gpuBudgetGuard\":false,\"tscale\":\"oversample\",\"hdrOutput\":\"auto\",\"customShaders\":[],\"aiBackend\":\"auto\",\"upscaleMode\":\"off\",\"upscaleParams\":{\"preHeight\":\"auto\",\"preHeightVersion\":2,\"maxHeight\":0,\"gpuThreads\":2,\"fp16\":true},\"interpolation\":{\"mode\":\"off\",\"factor\":\"auto\",\"preHeight\":\"auto\",\"preHeightVersion\":2,\"scMode\":0,\"scModeVersion\":2,\"gpuThreads\":2,\"mvRecal\":true,\"mvBlock\":true,\"rifeModel\":\"46\",\"rifeMode\":\"fast\",\"rifeScMode\":1,\"drbaModel\":2,\"drbaTurbo\":2,\"drbaTurboTrt\":1,\"amfProfile\":\"auto\",\"amfMvSearch\":\"native\"}},\n"
           "  \"audio\": {\"device\":\"\",\"exclusive\":false,\"channels\":\"stereo\",\"spdif\":\"\",\"volume\":100,\"mute\":false,\"masterClock\":\"auto\"},\n"
           "  \"playback\": {\"seekMode\":\"exact\"},\n"
#endif
           "  \"libraries\": [\n"
           "    {\"id\": \"movieA\", \"name\": \"Movie A\", \"kind\": \"movie\", \"folders\": []},\n"
           "    {\"id\": \"movieB\", \"name\": \"Movie B\", \"kind\": \"movie\", \"folders\": []},\n"
           "    {\"id\": \"series\", \"name\": \"Series\", \"kind\": \"series\", \"folders\": []},\n"
           "    {\"id\": \"live\", \"name\": \"Live\", \"kind\": \"live\", \"folders\": []}\n"
           "  ]\n"
           "}\n";
}

// config.json exactly as stored on disk (shared by both editions).
std::string LoadRawConfigText() {
    std::string text = ReadFileUtf8(ConfigPath());
    bool ok = false;
    vp::Json::Parse(text, &ok);
    if (!ok) {
        text = DefaultConfig();
        if (!fs::exists(ConfigPath())) WriteFileUtf8(ConfigPath(), text);
    }
    return text;
}

// The configuration as this edition sees it (the MPV edition substitutes its own LAN section).
std::string LoadConfigText();
#if !defined(VP_ENGINE_MPV)
std::string LoadConfigText() { return LoadRawConfigText(); }
#endif

bool HasMember(const vp::Json& object, const std::string& key) {
    return std::any_of(object.members().begin(), object.members().end(), [&](const auto& member) { return member.first == key; });
}

#if defined(VP_ENGINE_MPV)
std::string LoadConfigText() {
    return vp::mpv_config_safety::MakeMpvViewText(LoadRawConfigText());
}

// Stores a config edited through the MPV view: its "lan" becomes "mpvLan", and the PlaybackCore
// edition's "lan" is restored from disk so the MPV edition can never change it.
bool WriteMpvConfigText(const std::string& view_text) {
    std::string stored;
    return vp::mpv_config_safety::BuildStoredMpvConfigText(LoadRawConfigText(), view_text, &stored) &&
           WriteFileUtf8(ConfigPath(), stored);
}

bool EnsureMpvConfigKeys() {
    const fs::path path = ConfigPath();
    std::string text = ReadFileUtf8(path);
    if (text.empty()) return true; // LoadConfigText creates an MPV-aware default when absent.
    bool valid = false;
    const vp::Json current = vp::Json::Parse(text, &valid);
    if (!valid || !current.is_object()) return false;
    std::vector<std::pair<std::string, std::string>> additions;
    // DRBA interpolation.scMode (audit 7.5a): new configs get the upstream default 0 (k7sfunc DRBA_DML/DRBA_NV, mpv_PlayKit)
    // with scModeVersion 2. This also applies when a config first gains the "video" section here (a config written before
    // the MPV edition): it has never run DRBA in this edition, so 0 changes no behaviour its user has seen. Every config
    // that already has "video" is left as stored; a missing scMode there still means 1 (MpvBackend / app.js fallback).
    if (!HasMember(current, "video"))
        additions.emplace_back("video", "{\"gpuApi\":\"d3d11\",\"gpuAdapterLuid\":\"\",\"hwdec\":\"d3d11va\",\"preset\":\"custom\",\"restore\":\"off\",\"upscaler\":\"off\",\"sharpen\":\"off\",\"sharpenStrength\":1.0,\"deband\":\"off\",\"smooth\":\"off\",\"gpuBudgetGuard\":false,\"tscale\":\"oversample\",\"hdrOutput\":\"auto\",\"customShaders\":[],\"aiBackend\":\"auto\",\"upscaleMode\":\"off\",\"upscaleParams\":{\"preHeight\":\"auto\",\"preHeightVersion\":2,\"maxHeight\":0,\"gpuThreads\":2,\"fp16\":true},\"interpolation\":{\"mode\":\"off\",\"factor\":\"auto\",\"preHeight\":\"auto\",\"preHeightVersion\":2,\"scMode\":0,\"scModeVersion\":2,\"gpuThreads\":2,\"mvRecal\":true,\"mvBlock\":true,\"rifeModel\":\"46\",\"rifeMode\":\"fast\",\"rifeScMode\":1,\"drbaModel\":2,\"drbaTurbo\":2,\"drbaTurboTrt\":1,\"amfProfile\":\"auto\",\"amfMvSearch\":\"native\"}}");
    if (!HasMember(current, "audio"))
        additions.emplace_back("audio", "{\"device\":\"\",\"exclusive\":false,\"channels\":\"stereo\",\"spdif\":\"\",\"volume\":100,\"mute\":false,\"masterClock\":\"auto\"}");
    if (additions.empty()) return true;

    const fs::path backup = g.root / L"config.json.pre-mpv-20261011";
    if (!fs::exists(backup)) {
        if (!CopyFileW(path.c_str(), backup.c_str(), TRUE)) return false;
    }
    const std::size_t close = text.find_last_of('}');
    if (close == std::string::npos) return false;
    std::string inserted;
    if (!current.members().empty()) inserted = ",\n";
    for (std::size_t i = 0; i < additions.size(); ++i) {
        if (i) inserted += ",\n";
        inserted += "  " + vp::Json::Quote(additions[i].first) + ": " + additions[i].second;
    }
    text.insert(close, inserted + "\n");
    return WriteFileUtf8(path, text);
}
#endif

void StopAudioHost() {
    if (g.audio_process.hProcess) {
        TerminateProcess(g.audio_process.hProcess, 0);
        WaitForSingleObject(g.audio_process.hProcess, 2000);
        CloseHandle(g.audio_process.hProcess);
        CloseHandle(g.audio_process.hThread);
        g.audio_process = {};
    }
    g.audio_status = "stopped";
}

std::string AudioHostExe(const vp::Json& config) {
    std::string exe = config["audioHostExe"].str();
    if (!exe.empty() && fs::exists(Wide(exe))) return exe;
    // Default layout: <root>/audio-host/PlaybackCore.AudioHost.exe (build.ps1 copies it there).
    const fs::path fallback = g.root / L"audio-host" / L"PlaybackCore.AudioHost.exe";
    return fs::exists(fallback) ? Utf8(fallback.wstring()) : std::string();
}

void StartAudioHost() {
    StopAudioHost();
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    const std::string driver = config["asioDriver"].str();
    std::string backend = config["audioBackend"].str();
    if (backend.empty()) backend = "asio";
    std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string wasapi_device = config["wasapiDeviceId"].str();
    const bool wasapi_exclusive = config["wasapiExclusive"].boolean();
    const std::string exe = AudioHostExe(config);
    if (exe.empty()) { g.audio_status = "no-exe"; return; }
    if (backend != "asio" && backend != "wasapi") { g.audio_status = "invalid-backend"; return; }
    if (backend == "asio" && driver.empty()) { g.audio_status = "no-driver"; return; }

    if (!g.audio_job) {
        g.audio_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g.audio_job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    std::error_code ec;
    fs::create_directories(DataDir() / L"logs", ec);
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileW((DataDir() / L"logs" / L"audio-host.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = nullptr;
    std::wstring command = QuoteProcessArgument(Wide(exe)) + L" --backend " + QuoteProcessArgument(Wide(backend));
    if (backend == "asio") command += L" --driver " + QuoteProcessArgument(Wide(driver));
    else command += L" --device-id " + QuoteProcessArgument(Wide(wasapi_device)) +
                    L" --exclusive " + QuoteProcessArgument(wasapi_exclusive ? L"true" : L"false");
    const fs::path working = fs::path(Wide(exe)).parent_path();
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        working.c_str(), &startup, &g.audio_process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!created) { g.audio_status = "start-failed"; g.audio_process = {}; return; }
    AssignProcessToJobObject(g.audio_job, g.audio_process.hProcess);
    ResumeThread(g.audio_process.hThread);
    g.audio_status = "running";
}

std::string AudioStatusJson() {
#if defined(VP_ENGINE_MPV)
    return "\"audioHost\":\"engine\",\"audioEngine\":\"wasapi\"";
#else
    if (g.audio_process.hProcess && WaitForSingleObject(g.audio_process.hProcess, 0) == WAIT_OBJECT_0) g.audio_status = "exited";
    return "\"audioHost\":" + vp::Json::Quote(g.audio_status);
#endif
}

// Runs `AudioHost --list` and returns the driver names (one per line, "<name>  {clsid}").
void ListAsioDrivers(vp::Json request) {
    std::thread([request]() {
        const vp::Json config = vp::Json::Parse(LoadConfigText());
        const std::string exe = AudioHostExe(config);
        std::string names = "[";
        if (!exe.empty()) {
            SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE read_pipe{}, write_pipe{};
            if (CreatePipe(&read_pipe, &write_pipe, &inherit, 0)) {
                SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{sizeof(STARTUPINFOW)};
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write_pipe;
                startup.hStdError = write_pipe;
                PROCESS_INFORMATION process{};
                std::wstring command = L"\"" + Wide(exe) + L"\" --list";
                if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
                    CloseHandle(write_pipe);
                    write_pipe = nullptr;
                    std::string output;
                    char buffer[4096];
                    DWORD got = 0;
                    while (ReadFile(read_pipe, buffer, sizeof buffer, &got, nullptr) && got) output.append(buffer, got);
                    WaitForSingleObject(process.hProcess, 5000);
                    CloseHandle(process.hProcess);
                    CloseHandle(process.hThread);
                    std::istringstream lines(output);
                    std::string line;
                    bool first = true;
                    while (std::getline(lines, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        const auto brace = line.rfind("  {");
                        if (brace == std::string::npos) continue;
                        names += (first ? "" : ",") + vp::Json::Quote(line.substr(0, brace));
                        first = false;
                    }
                }
                if (write_pipe) CloseHandle(write_pipe);
                CloseHandle(read_pipe);
            }
        }
        names += "]";
        PostToWebFromAnyThread(Reply("asioDrivers", request, "\"drivers\":" + names));
    }).detach();
}

void ListWasapiDevices(vp::Json request) {
    std::thread([request]() {
        const vp::Json config = vp::Json::Parse(LoadConfigText());
        const std::string exe = AudioHostExe(config);
        std::string devices = "[]";
        if (!exe.empty()) {
            SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE read_pipe{}, write_pipe{};
            if (CreatePipe(&read_pipe, &write_pipe, &inherit, 0)) {
                SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{sizeof(STARTUPINFOW)};
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write_pipe;
                startup.hStdError = write_pipe;
                startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                PROCESS_INFORMATION process{};
                std::wstring command = QuoteProcessArgument(Wide(exe)) + L" --list-wasapi";
                if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
                    CloseHandle(write_pipe);
                    write_pipe = nullptr;
                    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0) {
                        std::string output;
                        char buffer[4096];
                        DWORD got = 0;
                        while (ReadFile(read_pipe, buffer, sizeof buffer, &got, nullptr) && got) output.append(buffer, got);
                        bool valid = false;
                        const vp::Json parsed = vp::Json::Parse(output, &valid);
                        if (valid && parsed.is_array()) devices = std::move(output);
                    } else {
                        TerminateProcess(process.hProcess, 1);
                        WaitForSingleObject(process.hProcess, 1000);
                    }
                    CloseHandle(process.hProcess);
                    CloseHandle(process.hThread);
                }
                if (write_pipe) CloseHandle(write_pipe);
                CloseHandle(read_pipe);
            }
        }
        PostToWebFromAnyThread(Reply("wasapiDevices", request, "\"devices\":" + devices));
    }).detach();
}

// ---------------------------------------------------------------- library scan
bool LanUserCanReadLibrary(const std::string& user_id, const std::string& library_id);
bool LanUserCanReadPath(const std::string& user_id, const fs::path& file);
bool LanUserCanDelete(const std::string& user_id);

bool IsVideoFile(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    static const wchar_t* kExtensions[] = {L".mkv", L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".ts",  L".m2ts",
                                           L".mts", L".webm", L".flv", L".mpg", L".mpeg", L".vob", L".rmvb"};
    return std::any_of(std::begin(kExtensions), std::end(kExtensions), [&](const wchar_t* e) { return ext == e; });
}

// Kodi / Emby / Jellyfin style side-car metadata: .nfo files and local artwork.
bool IsSidecarFile(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext == L".nfo" || ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp" || ext == L".tbn";
}

// Lists video files (and side-car subtitles) under each library folder on a worker thread.
void ScanLibraries(vp::Json request) {
    std::thread([request]() {
        const vp::Json config = vp::Json::Parse(LoadConfigText());
        std::string out = "[";
        bool first_library = true;
        std::size_t total = 0;
        for (const vp::Json& library : config["libraries"].items()) {
            const std::string user_id = request["__user"].str("default");
            if (LanSession(request) && !LanUserCanReadLibrary(user_id, library["id"].str())) continue;
            out += std::string(first_library ? "" : ",") + "{\"id\":" + vp::Json::Quote(library["id"].str()) + ",\"files\":[";
            first_library = false;
            bool first_file = true;
            std::map<std::wstring, std::vector<std::wstring>> sidecars; // folder -> nfo / image file names
            for (const vp::Json& folder : library["folders"].items()) {
                const fs::path base = Wide(folder.str());
                std::error_code ec;
                if (base.empty() || !fs::is_directory(base, ec)) continue;
                fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
                for (; !ec && it != end && total < 50000; it.increment(ec)) {
                    if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
                    std::error_code fec;
                    if (it->is_regular_file(fec) && IsSidecarFile(it->path())) {
                        auto& names = sidecars[it->path().parent_path().wstring()];
                        if (names.size() < 400) names.push_back(it->path().filename().wstring());
                        continue;
                    }
                    if (!it->is_regular_file(fec) || !IsVideoFile(it->path())) continue;
                    const auto size = it->file_size(fec);
                    const auto written = it->last_write_time(fec);
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        written.time_since_epoch() - std::chrono::seconds(11644473600LL)) // FILETIME epoch -> Unix epoch
                                        .count();
                    std::string subtitles = "[";
                    {
                        // Side-car subtitles: <stem>*.srt|ass|ssa|sup|vtt in the same folder.
                        const std::wstring stem = it->path().stem().wstring();
                        bool first_sub = true;
                        std::error_code sec;
                        for (const auto& sibling : fs::directory_iterator(it->path().parent_path(), sec)) {
                            std::wstring ext = sibling.path().extension().wstring();
                            for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
                            if (ext != L".srt" && ext != L".ass" && ext != L".ssa" && ext != L".sup" && ext != L".vtt") continue;
                            if (sibling.path().stem().wstring().rfind(stem, 0) != 0) continue;
                            subtitles += (first_sub ? "" : ",") + vp::Json::Quote(Utf8(sibling.path().wstring()));
                            first_sub = false;
                        }
                    }
                    subtitles += "]";
                    out += std::string(first_file ? "" : ",") + "{\"path\":" + vp::Json::Quote(Utf8(it->path().wstring())) +
                           ",\"rel\":" + vp::Json::Quote(Utf8(fs::relative(it->path(), base, fec).wstring())) +
                           ",\"root\":" + vp::Json::Quote(Utf8(base.wstring())) + ",\"size\":" + std::to_string(size) +
                           ",\"mtime\":" + std::to_string(ms) + ",\"subtitles\":" + subtitles + "}";
                    first_file = false;
                    ++total;
                }
            }
            out += "],\"sidecars\":{";
            bool first_dir = true;
            for (const auto& [dir, names] : sidecars) {
                out += std::string(first_dir ? "" : ",") + vp::Json::Quote(Utf8(dir)) + ":[";
                for (std::size_t i = 0; i < names.size(); ++i) out += (i ? "," : "") + vp::Json::Quote(Utf8(names[i]));
                out += "]";
                first_dir = false;
            }
            out += "}}";
        }
        out += "]";
        ReplyTo(request, Reply("library", request, "\"libraries\":" + out));
    }).detach();
}

// ---------------------------------------------------------------- dialogs
std::string PickPath(bool folder, bool subtitle = false) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"影片", L"*.mkv;*.mp4;*.m4v;*.mov;*.avi;*.wmv;*.ts;*.m2ts;*.mts;*.webm;*.flv;*.mpg;*.mpeg;*.vob;*.rmvb"},
            {L"字幕", L"*.srt;*.ass;*.ssa;*.vtt;*.sup;*.sub"},
            {L"所有檔案", L"*.*"}};
        dialog->SetFileTypes(3, filters);
        if (subtitle) dialog->SetFileTypeIndex(2);
    }
    if (FAILED(dialog->Show(g.window))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::string result = Utf8(path);
    CoTaskMemFree(path);
    return result;
}

// ---------------------------------------------------------------- window helpers
void SetFullscreen(bool on) {
    if (on == g.fullscreen) return;
    // Announced before the resize so the UI can put its black curtain over the video for the
    // frames in which the WebView and the video child are re-laid out; the event without
    // "pending" follows once the window has its new size.
    PostToWeb(std::string("{\"type\":\"fullscreen\",\"on\":") + (on ? "true" : "false") + ",\"pending\":true}");
    const DWORD style = static_cast<DWORD>(GetWindowLongW(g.window, GWL_STYLE));
    if (on) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetWindowPlacement(g.window, &g.placement);
        GetMonitorInfoW(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongW(g.window, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(g.window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g.window, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        // One resize: SetWindowPlacement moves/sizes the window, and that size change already
        // recalculates the frame for the restored style. The extra FRAMECHANGED SetWindowPos that
        // used to follow re-laid out the WebView and the video child a second time.
        SetWindowPlacement(g.window, &g.placement);
        RECT window_rect{}, client_rect{};
        GetWindowRect(g.window, &window_rect);
        GetClientRect(g.window, &client_rect);
        // Only if the restored rect equals the fullscreen one (no size change, so no WM_NCCALCSIZE).
        if (client_rect.right == window_rect.right - window_rect.left && client_rect.bottom == window_rect.bottom - window_rect.top)
            SetWindowPos(g.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g.fullscreen = on;
    PostToWeb(std::string("{\"type\":\"fullscreen\",\"on\":") + (on ? "true" : "false") + "}");
}

void ShowVideoSurface(bool show) {
    g.video_shown = show;
#if defined(VP_ENGINE_MPV)
    if (g.video_child && IsWindow(g.video_child)) ShowWindow(g.video_child, show ? SW_SHOWNA : SW_HIDE);
#else
    if (!g.video_visual) return;
    if (show && g.player) {
        void* raw = nullptr;
        if (pc_get_swap_chain(g.player, &raw) == PC_OK && raw) {
            auto* swap_chain = static_cast<IDXGISwapChain1*>(raw);
            g.video_visual->SetContent(swap_chain);
            swap_chain->Release(); // the visual keeps its own reference
        }
    } else {
        g.video_visual->SetContent(nullptr);
    }
    g.dcomp->Commit();
#endif
}

// The UI's `releaseSurface` (sent after its exit transition) and the LAN page's `stop`.
void ReleaseVideoSurface() {
#if defined(VP_ENGINE_MPV)
    if (g.player && g.player->Ready()) g.player->ReleaseSurface();
#endif
    ShowVideoSurface(false);
}

// What the main window paints behind the transparent WebView: black while a video owns the
// window, so areas exposed by a resize (fullscreen switch) do not flash the light ground.
bool VideoSurfaceInUse() {
#if defined(VP_ENGINE_MPV)
    return (g.video_child && IsWindowVisible(g.video_child)) || (g.player && g.player->Ready() && g.player->MediaActive());
#else
    return g.video_shown;
#endif
}

void OnResize() {
    RECT client{};
    GetClientRect(g.window, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    if (g.controller) g.controller->put_Bounds(client);
#if defined(VP_ENGINE_MPV)
    if (g.video_child && IsWindow(g.video_child)) {
        SetWindowPos(g.video_child, HWND_BOTTOM, 0, 0, static_cast<int>(width), static_cast<int>(height),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (g.player && g.player->Ready()) g.player->Resize(static_cast<int>(width), static_cast<int>(height));
    }
#else
    if (g.player) pc_resize(g.player, width, height);
#endif
    if (g.dcomp) g.dcomp->Commit();
}

// ---------------------------------------------------------------- player
#if !defined(VP_ENGINE_MPV)
const char* PlayerEventName(int event) {
    switch (event) {
    case PC_EVENT_OPENED: return "opened";
    case PC_EVENT_STATE: return "state";
    case PC_EVENT_POSITION: return "position";
    case PC_EVENT_CLOCK: return "clock";
    case PC_EVENT_ERROR: return "error";
    case PC_EVENT_SURFACE: return "surface";
    case PC_EVENT_ENDED: return "ended";
    case PC_EVENT_SUBTITLE: return "subtitle";
    case PC_EVENT_FRAME_PROCESSING: return "frameProcessing";
    default: return "unknown";
    }
}

// Render thread -> UI thread.
void PC_CALL OnPlayerEvent(void*, int32_t event, const char* json) {
    auto* payload = new std::string(json ? json : "{}");
    if (!PostMessageW(g.window, WM_APP_PLAYER, static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(payload))) delete payload;
}

void HandlePlayerEvent(int event, const std::string& json) {
    if (event == PC_EVENT_SURFACE) ShowVideoSurface(true);
    if (event == PC_EVENT_STATE) {
        const vp::Json state = vp::Json::Parse(json);
        const std::string name = state["state"].str();
        if (name == "stopped" || name == "error") ShowVideoSurface(false);
    }
    PostToWeb(std::string("{\"type\":\"player\",\"event\":\"") + PlayerEventName(event) + "\",\"data\":" + json + "}");
}
#endif

#if defined(VP_ENGINE_MPV)
// Core-reliability 1A (report 25-A §3.4): a stale LUID reconciled by its unique identity, or a saved LUID without an
// identity (legacy config), is written back once: video.gpuAdapterLuid + video.gpuAdapterIdentity, through the same
// PlaybackCore guard as every MPV save. Returns the gpuAdapterStatus "persisted" member, or "".
std::string PersistGpuAdapterSelection() {
    vp::MpvBackend::AdapterPersist persist;
    if (!g.player || !g.player->TakeAdapterPersist(&persist)) return {};
    const std::string luid = std::to_string(persist.adapter.luid);
    const std::string identity = vp::gpu::IdentityJson(persist.adapter);
    std::string updated;
    const bool written = vp::gpu::SetVideoGpuAdapterText(LoadConfigText(), luid, identity, &updated) && WriteMpvConfigText(updated);
    const std::string result = written ? "yes" : "no";
    g.player->HostLog(persist.outcome == "reconciled"
        ? "gpu adapter reconciled old_luid=" + persist.old_luid + " new_luid=" + luid + " key=vendor/device/subsys/rev persisted=" + result
        : "gpu adapter identity backfilled luid=" + luid + " source=" + persist.source + " persisted=" + result);
    return written ? "{\"luid\":" + vp::Json::Quote(luid) + ",\"identity\":" + identity + "}" : std::string();
}

// gpuAdapterStatus push: only when the preflight result changed (blocked EnsurePlayer calls repeat it), or with a persist.
std::string g_gpu_status_posted;
void PostGpuAdapterStatus(const std::string& persisted) {
    if (!g.player) return;
    const std::string status = g.player->AdapterStatusJson();
    if (status.empty() || (persisted.empty() && status == g_gpu_status_posted)) return;
    g_gpu_status_posted = status;
    PostToWeb("{\"type\":\"gpuAdapterStatus\",\"status\":" + status + (persisted.empty() ? std::string() : ",\"persisted\":" + persisted) + "}");
}
#endif

void EnsurePlayer() {
#if defined(VP_ENGINE_MPV)
    if (g.player && (g.player->Ready() || g.player->InitializationAttempted())) return;
    if (!g.player) g.player = std::make_unique<vp::MpvBackend>();
    std::string error;
    const bool ready = g.player->Initialize(g.window, g.video_child, ExeDir(), DataDir(), LoadConfigText(),
        [] () -> unsigned long { // 1B: report the failure (the backend retries); never posts to a null window
            const HWND window = g.window;
            if (!window) return ERROR_INVALID_WINDOW_HANDLE;
            return PostMessageW(window, WM_APP_MPV_WAKE, 0, 0) ? 0ul : GetLastError();
        },
        [](const std::string& event, const std::string& json) {
            PostToWeb("{\"type\":\"player\",\"event\":" + vp::Json::Quote(event) + ",\"data\":" + json + "}");
        }, &error);
    if (!ready) {
        g.player_error = error.empty() ? "libmpv initialization failed." : error;
        g.player_error_code = g.player->ErrorCode().empty() ? "MPV_INIT" : g.player->ErrorCode();
    } else { g.player_error.clear(); g.player_error_code.clear(); OnResize(); }
    PostGpuAdapterStatus(PersistGpuAdapterSelection()); // 1A: blocked / reconciled / ok, to the banner and settings
    RecordCrashModule(GetModuleHandleW(L"libmpv-2.dll"), true); // first-chance tripwire scope (CrashVectoredHandler)
#else
    if (g.player) return;
    g.player = pc_create();
    pc_set_event_callback(g.player, OnPlayerEvent, nullptr);
    pc_set_cache_dir(g.player, Utf8((DataDir() / L"cache").wstring()).c_str());
    RECT client{};
    GetClientRect(g.window, &client);
    pc_set_render_target(g.player, PC_TARGET_COMPOSITION, nullptr, static_cast<uint32_t>(std::max<LONG>(1, client.right)),
                         static_cast<uint32_t>(std::max<LONG>(1, client.bottom)));
#endif
}

#if !defined(VP_ENGINE_MPV)
// {"mode":"off"|"anime4k"|"animejanai_l1",
//  "animejanaiModel":"v2_performance"|"v2_balanced"|"v3_performance"|"v3_balanced",
//  "inferenceHeights":{"<model>":720|1080,...},"inferenceHeight":720|1080}
// from config.json or a live setUpscale message. Each AnimeJaNai model keeps its
// own cap; older configs (single inferenceHeight, no model) mean V2 at that cap.
int32_t ApplyUpscaleConfig(const vp::Json& value, bool force_off = false) {
    if (!g.player) return PC_E_STATE;
    const std::string mode = value["mode"].str();
    const int32_t upscale_mode = force_off ? PC_UPSCALE_OFF : mode == "anime4k" ? PC_UPSCALE_ANIME4K_ACNET
        : mode == "animejanai_l1" ? PC_UPSCALE_ANIMEJANAI_V2_L1
        : PC_UPSCALE_OFF;
    const std::string model_id = value["animejanaiModel"].str("v2_performance");
    const int32_t animejanai_model = model_id == "v3_balanced" ? PC_ANIMEJANAI_HD_V3_BALANCED
        : model_id == "v2_balanced" ? PC_ANIMEJANAI_V2_BALANCED
        : model_id == "v3_performance" ? PC_ANIMEJANAI_HD_V3_PERFORMANCE
        : PC_ANIMEJANAI_V2_PERFORMANCE;
    std::int64_t inference_height = value["inferenceHeights"][model_id].i64(0);
    if (inference_height != 720 && inference_height != 1080) inference_height = value["inferenceHeight"].i64(720);
    if (inference_height != 720 && inference_height != 1080) return PC_E_INVALID_ARG;
    const int32_t model_result = pc_set_animejanai_model(g.player, animejanai_model);
    if (model_result != PC_OK) return model_result;
    return pc_set_upscale_ex(g.player, upscale_mode, static_cast<int32_t>(inference_height));
}
#endif

// ---------------------------------------------------------------- segment thumbnails
// Two independent slots: "segments" (5/10/15-minute cards on the detail page) and
// "trickplay" (dense images for the seek-bar preview while playing). A new request in a
// slot cancels the previous one in that slot.
std::atomic<std::uint64_t> g_thumb_jobs[5]; // segments (chapters), trickplay, covers, resume frames, seek-bar scrub frames
std::atomic<std::uint64_t> g_lan_thumb_jobs[5]; // the same slots for LAN clients (they never cancel local jobs)
std::mutex g_thumb_media_mutex;
std::map<std::string, std::string> g_thumb_media;
std::atomic<std::uint64_t>& ThumbCounter(std::uint64_t session, int slot) { return session ? g_lan_thumb_jobs[slot] : g_thumb_jobs[slot]; }

struct ThumbJob {
    int slot;
    std::uint64_t id;
    std::string media;
    std::string folder; // thumbs.local/<folder>/
    std::int64_t interval_ms;
    std::uint64_t session = 0; // LAN client, 0 = local page
};

#if defined(VP_ENGINE_MPV)
constexpr int32_t PC_THUMB_INFO = 1;
constexpr int32_t PC_THUMB_IMAGE = 2;
#endif

const char* SlotName(int slot) { return slot == 1 ? "trickplay" : slot == 2 ? "covers" : slot == 3 ? "resume" : slot == 4 ? "scrub" : "segments"; }
int SlotIndex(const std::string& name) { return name == "trickplay" ? 1 : name == "covers" ? 2 : name == "resume" ? 3 : name == "scrub" ? 4 : 0; }

#if defined(VP_ENGINE_MPV)
int32_t OnThumbnail(void* user, int32_t kind, int64_t value_ms, const char* text) {
#else
int32_t PC_CALL OnThumbnail(void* user, int32_t kind, int64_t value_ms, const char* text) {
#endif
    auto* job = static_cast<ThumbJob*>(user);
    if (ThumbCounter(job->session, job->slot).load() != job->id) return 1; // superseded: cancel
    std::string fields = "\"slot\":\"" + std::string(SlotName(job->slot)) + "\",\"path\":" + vp::Json::Quote(job->media) +
                         ",\"intervalMs\":" + std::to_string(job->interval_ms);
    if (kind == PC_THUMB_INFO) fields += ",\"kind\":\"info\",\"durationMs\":" + std::to_string(value_ms);
    else if (kind == PC_THUMB_IMAGE) {
        if (job->interval_ms == 0) fields += ",\"exact\":true";
        const std::string file = Utf8(fs::path(Wide(text ? text : "")).filename().wstring());
        fields += ",\"kind\":\"image\",\"ms\":" + std::to_string(value_ms) + ",\"url\":" +
                  vp::Json::Quote("https://thumbs.local/" + job->folder + "/" + file);
    } else fields += ",\"kind\":\"error\",\"ms\":" + std::to_string(value_ms) + ",\"message\":" + vp::Json::Quote(text ? text : "");
    const std::string message = "{\"type\":\"thumb\"," + fields + "}";
    if (job->session) g_lan.Push(job->session, LanRewrite(message));
    else PostToWebFromAnyThread(message);
    return 0;
}

std::string ThumbFolder(const std::string& media) {
    std::error_code ec;
    const fs::path file = Wide(media);
    const auto size = fs::file_size(file, ec);
    const auto written = fs::last_write_time(file, ec).time_since_epoch().count();
    // One folder per file version; every interval and exact time shares it.
    const std::uint64_t key = std::hash<std::string>{}(media + "|" + std::to_string(size) + "|" + std::to_string(written));
    char folder[32];
    std::snprintf(folder, sizeof folder, "%016llx", static_cast<unsigned long long>(key));
    const std::string result = folder;
    { std::lock_guard<std::mutex> lock(g_thumb_media_mutex); g_thumb_media[result] = media; }
    return result;
}

// Exact frames at explicit times (chapter strips, the frame where playback stopped).
void StartThumbnailsAt(const vp::Json& message) {
    const int slot = SlotIndex(message["slot"].str());
    const std::uint64_t session = LanSession(message);
    const std::uint64_t id = ++ThumbCounter(session, slot);
    const std::string media = message["path"].str();
    std::vector<std::int64_t> times;
    for (const vp::Json& t : message["times"].items()) times.push_back(t.i64());
    const std::string tag = message["tag"].str();
    if (media.empty() || (session && !LanUserCanReadPath(message["__user"].str("default"), Wide(media)))) return;
    std::thread([slot, id, media, times, tag, session, message]() {
        const std::string folder = ThumbFolder(media);
        ThumbJob job{slot, id, media, folder, 0, session};
#if defined(VP_ENGINE_MPV)
        const std::string output = Utf8((DataDir() / L"thumbs" / Wide(folder)).wstring());
        const int result = vp::media_tools::GenerateThumbnailsAt(media, output, times, 384, false,
            [&](vp::media_tools::ThumbnailEvent kind, std::int64_t ms, const std::string& text) {
                return OnThumbnail(&job, static_cast<int32_t>(kind), ms, text.c_str());
            });
#else
        const int result = pc_generate_thumbnails_at(media.c_str(), Utf8((DataDir() / L"thumbs" / Wide(folder)).wstring()).c_str(), times.data(),
                                                     static_cast<uint32_t>(times.size()), 384, 0, OnThumbnail, &job);
#endif
        if (ThumbCounter(session, slot).load() == id)
            ReplyTo(message, "{\"type\":\"thumb\",\"slot\":\"" + std::string(SlotName(slot)) + "\",\"path\":" + vp::Json::Quote(media) +
                                   ",\"intervalMs\":0,\"exact\":true,\"tag\":" + vp::Json::Quote(tag) + ",\"kind\":\"done\",\"result\":" +
                                   std::to_string(result) + "}");
    }).detach();
}

void StartThumbnails(const vp::Json& message) {
    const int slot = SlotIndex(message["slot"].str());
    const std::uint64_t session = LanSession(message);
    const std::uint64_t id = ++ThumbCounter(session, slot);
    const std::string media = message["path"].str();
    std::int64_t interval = message["intervalMs"].i64(600000);
    if (media.empty() || (session && !LanUserCanReadPath(message["__user"].str("default"), Wide(media)))) return;
    std::thread([slot, id, media, interval, session, message]() {
        const std::string folder = ThumbFolder(media);
        ThumbJob job{slot, id, media, folder, interval, session};
#if defined(VP_ENGINE_MPV)
        const std::string output = Utf8((DataDir() / L"thumbs" / Wide(folder)).wstring());
        const int result = vp::media_tools::GenerateThumbnails(media, output, interval, slot == 1 ? 240 : 384,
            [&](vp::media_tools::ThumbnailEvent kind, std::int64_t ms, const std::string& text) {
                return OnThumbnail(&job, static_cast<int32_t>(kind), ms, text.c_str());
            });
#else
        const int result = pc_generate_thumbnails(media.c_str(), Utf8((DataDir() / L"thumbs" / Wide(folder)).wstring()).c_str(), interval,
                                                  slot == 1 ? 240 : 384, OnThumbnail, &job);
#endif
        if (ThumbCounter(session, slot).load() == id)
            ReplyTo(message, "{\"type\":\"thumb\",\"slot\":\"" + std::string(SlotName(slot)) + "\",\"path\":" +
                                   vp::Json::Quote(media) + ",\"intervalMs\":" + std::to_string(interval) + ",\"kind\":\"done\",\"result\":" +
                                   std::to_string(result) + "}");
    }).detach();
}

// ---------------------------------------------------------------- media.local: library files for the page
// https://media.local/f?p=<UTF-8 path, percent-encoded> serves .nfo files and artwork that live
// in the user's library folders (only there, and only those types).
std::string PercentDecode(const std::wstring& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'%' && i + 2 < text.size() && std::iswxdigit(text[i + 1]) && std::iswxdigit(text[i + 2])) {
            out += static_cast<char>(std::stoi(std::wstring(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else if (c == L'+') {
            out += ' ';
        } else {
            out += static_cast<char>(c < 0x80 ? c : '?');
        }
    }
    return out;
}

std::wstring Lower(std::wstring text) {
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

const wchar_t* MediaMime(const fs::path& path) {
    const std::wstring ext = Lower(path.extension().wstring());
    if (ext == L".nfo") return L"text/xml; charset=utf-8";
    if (ext == L".png") return L"image/png";
    if (ext == L".webp") return L"image/webp";
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".tbn") return L"image/jpeg";
    return nullptr;
}

bool InsideLibrary(const fs::path& file) {
    std::error_code ec;
    const std::wstring target = Lower(fs::weakly_canonical(file, ec).wstring());
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    for (const vp::Json& library : config["libraries"].items())
        for (const vp::Json& folder : library["folders"].items()) {
            std::wstring base = Lower(fs::weakly_canonical(fs::path(Wide(folder.str())), ec).wstring());
            if (base.empty()) continue;
            if (base.back() != L'\\') base += L'\\';
            if (target.rfind(base, 0) == 0) return true;
        }
    return false;
}

void ServeMediaFile(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    LPWSTR raw = nullptr;
    request->get_Uri(&raw);
    const std::wstring uri = raw ? raw : L"";
    CoTaskMemFree(raw);
    const auto at = uri.find(L"p=");
    const fs::path file = at == std::wstring::npos ? fs::path() : fs::path(Wide(PercentDecode(uri.substr(at + 2))));
    const wchar_t* mime = file.empty() ? nullptr : MediaMime(file);
    ComPtr<ICoreWebView2WebResourceResponse> response;
    ComPtr<IStream> stream;
    if (mime && InsideLibrary(file) &&
        SUCCEEDED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
        const std::wstring headers = std::wstring(L"Content-Type: ") + mime + L"\r\nCache-Control: max-age=600\r\nAccess-Control-Allow-Origin: *";
        g.environment->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    } else {
        g.environment->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"Access-Control-Allow-Origin: *", &response);
    }
    if (response) args->put_Response(response.Get());
}

// Cover art inside a media file (Matroska attachments / MP4 attached pictures), extracted once.
void ExtractArtwork(vp::Json request) {
    std::thread([request]() {
        const std::string media = request["path"].str();
        if (LanSession(request) && !LanUserCanReadPath(request["__user"].str("default"), Wide(media))) return;
        const fs::path dir = DataDir() / L"thumbs" / Wide(ThumbFolder(media)) / L"art";
        const std::string folder = Utf8((fs::path(Wide(ThumbFolder(media))) / L"art").generic_wstring());
        std::error_code ec;
        if (!fs::exists(dir / L"artwork.done", ec)) {
#if defined(VP_ENGINE_MPV)
            vp::media_tools::ExtractArtwork(media, Utf8(dir.wstring()),
                [](vp::media_tools::ThumbnailEvent, std::int64_t, const std::string&) { return 0; });
#else
            struct Ignore { static int32_t PC_CALL Cb(void*, int32_t, int64_t, const char*) { return 0; } };
            pc_extract_artwork(media.c_str(), Utf8(dir.wstring()).c_str(), &Ignore::Cb, nullptr);
#endif
        }
        std::string images = "[";
        bool first = true;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name == L"artwork.done" || !MediaMime(entry.path())) continue;
            images += std::string(first ? "" : ",") + "{\"name\":" + vp::Json::Quote(Utf8(name)) + ",\"url\":" +
                      vp::Json::Quote("https://thumbs.local/" + folder + "/" + Utf8(name)) + "}";
            first = false;
        }
        images += "]";
        ReplyTo(request, "{\"type\":\"artwork\",\"path\":" + vp::Json::Quote(media) + ",\"images\":" + images + "}");
    }).detach();
}

// RETRY_AFTER_BEGIN (tests/host/retry-after-check.cpp compiles this block verbatim)
// Retry-After (RFC 7231 7.1.3) is either delta-seconds or an HTTP-date (IMF-fixdate, RFC 850 or asctime).
// Returns whole seconds from now, clamped to [0, 86400]; 0 when the value cannot be parsed, so the page's
// own default applies. The cap keeps a bogus far-future value from pausing a provider for days.
static int RetryAfterSeconds(const wchar_t* value) {
    constexpr long long kMax = 86400;
    if (!value) return 0;
    while (*value && iswspace(*value)) ++value;
    const wchar_t* p = value;
    long long seconds = 0;
    bool digits = false;
    while (*p >= L'0' && *p <= L'9') {
        digits = true;
        if (seconds < kMax) seconds = seconds * 10 + (*p - L'0');
        ++p;
    }
    while (*p && iswspace(*p)) ++p;
    if (digits && !*p) return static_cast<int>(std::min(seconds, kMax));
    if (!*value) return 0;
    SYSTEMTIME when = {}, now = {};
    FILETIME when_ft = {}, now_ft = {};
    if (!WinHttpTimeToSystemTime(value, &when) || !SystemTimeToFileTime(&when, &when_ft)) return 0;
    GetSystemTime(&now);
    if (!SystemTimeToFileTime(&now, &now_ft)) return 0;
    const auto ticks = [](const FILETIME& ft) {
        return static_cast<long long>((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    };
    const long long delta = (ticks(when_ft) - ticks(now_ft)) / 10000000LL; // 100 ns ticks -> whole seconds
    return static_cast<int>(std::clamp(delta, 0LL, kMax));
}
// RETRY_AFTER_END

// ---------------------------------------------------------------- metadata HTTP (AniList GraphQL, Bangumi)
// The page builds the requests; the host carries them so they do not depend on CORS. Only the
// metadata services below are reachable. Replies {status, retryAfter, text}.
void HttpRequest(vp::Json request) {
    std::thread([request]() {
        const std::string host = request["host"].str("graphql.anilist.co");
        const std::string method = request["method"].str("POST");
        const std::string path = request["path"].str("/");
        const std::string body = request["body"].str();
        const std::string extra = request["headers"].str(); // optional "Authorization: Bearer <token>" (Bangumi)
        int status = 0;
        int retry_after = 0;
        std::string text;
        const bool allowed = (host == "graphql.anilist.co" || host == "api.bgm.tv" || host == "api.themoviedb.org" || host == "kitsu.app") &&
                             (method == "GET" || method == "POST") &&
                             !path.empty() && path[0] == '/' &&
                             (extra.empty() || (extra.rfind("Authorization: Bearer ", 0) == 0 && extra.find_first_of("\r\n") == std::string::npos));
        HINTERNET session = allowed ? WinHttpOpen(L"VideoPlayer/1.0 (personal desktop media player)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
                                    : nullptr;
        HINTERNET connect = session ? WinHttpConnect(session, Wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET req = connect ? WinHttpOpenRequest(connect, Wide(method).c_str(), Wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
        if (req) {
            WinHttpSetTimeouts(req, 10000, 10000, 15000, 20000);
            // Kitsu speaks JSON:API and expects its media type.
            std::wstring headers = host == "kitsu.app" ? L"Accept: application/vnd.api+json\r\n" : L"Accept: application/json\r\n";
            if (method == "POST") headers += L"Content-Type: application/json\r\n";
            if (!extra.empty()) headers += Wide(extra) + L"\r\n";
            if (WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                status = static_cast<int>(code);
                // Delta-seconds or an HTTP-date (the RFC 850 form runs past 30 chars). A value that does not fit
                // (ERROR_INSUFFICIENT_BUFFER) is unparsable and stays 0, as does a missing header.
                wchar_t retry[64] = {};
                DWORD retry_size = sizeof(retry);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &retry_size, WINHTTP_NO_HEADER_INDEX)) retry_after = RetryAfterSeconds(retry);
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(req, &available) || !available) break;
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(req, chunk.data(), available, &read) || !read) break;
                    text.append(chunk.data(), read);
                    if (text.size() > 8 * 1024 * 1024) break;
                }
            }
        }
        if (req) WinHttpCloseHandle(req);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
        PostToWebFromAnyThread(Reply("http", request, "\"status\":" + std::to_string(status) + ",\"retryAfter\":" + std::to_string(retry_after) +
                                                        ",\"text\":" + vp::Json::Quote(text)));
    }).detach();
}

// ---------------------------------------------------------------- user images (custom posters)
// The page sends an image the user picked ({name, ext, data: base64}); it is written to
// data\cache\posters\<name>.<ext> and served as https://cache.local/posters/<name>.<ext>.
std::string DecodeBase64(const std::string& text) {
    static int table[256];
    static bool ready = false;
    if (!ready) {
        for (int& v : table) v = -1;
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = i;
        ready = true;
    }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    int buffer = 0, bits = 0;
    for (const unsigned char c : text) {
        if (c == '=') break;
        const int v = table[c];
        if (v < 0) continue; // whitespace / data: prefix leftovers
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits) & 0xFF); }
    }
    return out;
}

void SaveImage(const vp::Json& message) {
    const std::string name = message["name"].str();
    const std::string ext = message["ext"].str();
    const bool name_ok = !name.empty() && name.size() <= 64 &&
                         std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
    const bool ext_ok = ext == "jpg" || ext == "png" || ext == "webp";
    std::string url;
    if (name_ok && ext_ok) {
        const std::string bytes = DecodeBase64(message["data"].str());
        if (!bytes.empty() && bytes.size() <= 25 * 1024 * 1024) {
            const fs::path dir = DataDir() / L"cache" / L"posters";
            std::error_code ec;
            fs::create_directories(dir, ec);
            for (const char* old : {"jpg", "png", "webp"}) fs::remove(dir / Wide(name + "." + old), ec);
            std::ofstream out(dir / Wide(name + "." + ext), std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (out) url = "https://cache.local/posters/" + name + "." + ext;
        }
    }
    PostToWeb(Reply("imageSaved", message, "\"url\":" + vp::Json::Quote(url)));
}

void HandleWebMessage(const std::string& text);

// ---------------------------------------------------------------- LAN sharing: server side
// config.json "lan": {"mode": "standalone" | "server" | "client", "port": 47880, "name": "...",
//                     "server": "http://192.168.1.10:47880"}
// Server: other devices browse assigned libraries and stream files (see lan_server.h).
// Client: the window shows the server's UI; this PC only decodes and presents (no Anime4K, no
// frame interpolation) and accepts only playback commands from that page.

// The value of "key":"..." inside a JSON text replaced by "" (API keys never leave this PC).
std::string BlankJsonString(std::string text, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        std::size_t end = at + needle.size();
        while (end < text.size() && text[end] != '"') end += text[end] == '\\' ? 2 : 1;
        if (end >= text.size()) break;
        text.erase(at + needle.size(), end - (at + needle.size()));
    }
    return text;
}

struct LanUserProfile {
    std::string id;
    std::string name;
    std::vector<std::string> libraries;
    bool can_delete = false;
};

std::mutex& LanProgressMutex() {
    static std::mutex mutex;
    return mutex;
}

bool SafeLanUserId(const std::string& id) {
    return !id.empty() && id.size() <= 32 &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

std::vector<LanUserProfile> LanUsers() {
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    std::vector<std::string> all_libraries;
    for (const vp::Json& library : config["libraries"].items()) {
        const std::string id = library["id"].str();
        if (SafeLanUserId(id)) all_libraries.push_back(id);
    }
    const vp::Json saved = vp::Json::Parse(ReadFileUtf8(DataDir() / L"lan_users.json"));
    std::vector<LanUserProfile> users;
    if (saved.is_object()) {
        for (const vp::Json& value : saved["users"].items()) {
            LanUserProfile user;
            user.id = value["id"].str();
            user.name = value["name"].str();
            user.can_delete = value["canDelete"].boolean();
            if (!SafeLanUserId(user.id) || user.name.empty() || user.name.size() > 64) continue;
            for (const vp::Json& lib : value["libraries"].items()) {
                const std::string id = lib.str();
                if (std::find(all_libraries.begin(), all_libraries.end(), id) != all_libraries.end() &&
                    std::find(user.libraries.begin(), user.libraries.end(), id) == user.libraries.end()) user.libraries.push_back(id);
            }
            if (std::none_of(users.begin(), users.end(), [&](const LanUserProfile& item) { return item.id == user.id; })) users.push_back(std::move(user));
        }
    }
    auto default_user = std::find_if(users.begin(), users.end(), [](const LanUserProfile& user) { return user.id == "default"; });
    if (default_user == users.end()) users.insert(users.begin(), LanUserProfile{"default", "一般使用者", all_libraries, false});
    else if (default_user != users.begin()) std::rotate(users.begin(), default_user, default_user + 1);
    return users;
}

std::string LanUsersJson() {
    const auto users = LanUsers();
    std::string out = "{\"users\":[";
    for (std::size_t i = 0; i < users.size(); ++i) {
        const auto& user = users[i];
        out += std::string(i ? "," : "") + "{\"id\":" + vp::Json::Quote(user.id) + ",\"name\":" + vp::Json::Quote(user.name) + ",\"canDelete\":" +
               (user.can_delete ? "true" : "false") + ",\"libraries\":[";
        for (std::size_t j = 0; j < user.libraries.size(); ++j) out += std::string(j ? "," : "") + vp::Json::Quote(user.libraries[j]);
        out += "]}";
    }
    return out + "]}";
}

void EnsureLanUsersFile() {
    std::error_code ec;
    const fs::path path = DataDir() / L"lan_users.json";
    if (!fs::exists(path, ec)) WriteFileUtf8(path, LanUsersJson() + "\n");
}

bool LanUserCanReadLibrary(const std::string& user_id, const std::string& library_id) {
    const auto users = LanUsers();
    const auto it = std::find_if(users.begin(), users.end(), [&](const LanUserProfile& user) { return user.id == user_id; });
    return it != users.end() && std::find(it->libraries.begin(), it->libraries.end(), library_id) != it->libraries.end();
}

bool LanUserCanReadPath(const std::string& user_id, const fs::path& file) {
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    std::error_code ec;
    const std::wstring target = Lower(fs::weakly_canonical(file, ec).wstring());
    if (target.empty() || ec) return false;
    for (const vp::Json& library : config["libraries"].items()) {
        if (!LanUserCanReadLibrary(user_id, library["id"].str())) continue;
        for (const vp::Json& folder : library["folders"].items()) {
            ec.clear();
            std::wstring base = Lower(fs::weakly_canonical(fs::path(Wide(folder.str())), ec).wstring());
            if (ec || base.empty()) continue;
            if (base.back() != L'\\') base += L'\\';
            if (target.rfind(base, 0) == 0) return true;
        }
    }
    return false;
}

bool LanUserCanReadThumbnail(const std::string& user_id, const std::string& relative_path) {
    const auto slash = relative_path.find('/');
    const std::string folder = relative_path.substr(0, slash);
    std::string media;
    {
        std::lock_guard<std::mutex> lock(g_thumb_media_mutex);
        const auto it = g_thumb_media.find(folder);
        if (it == g_thumb_media.end()) return false;
        media = it->second;
    }
    return LanUserCanReadPath(user_id, fs::path(Wide(media)));
}

bool LanUserCanDelete(const std::string& user_id) {
    const auto users = LanUsers();
    const auto it = std::find_if(users.begin(), users.end(), [&](const LanUserProfile& user) { return user.id == user_id; });
    return it != users.end() && it->can_delete;
}

std::string LanUserLibrariesJson(const std::string& user_id) {
    const auto users = LanUsers();
    const auto it = std::find_if(users.begin(), users.end(), [&](const LanUserProfile& user) { return user.id == user_id; });
    std::string out = "[";
    if (it != users.end())
        for (std::size_t i = 0; i < it->libraries.size(); ++i) out += std::string(i ? "," : "") + vp::Json::Quote(it->libraries[i]);
    return out + "]";
}

std::string JsonText(const vp::Json& value) {
    switch (value.type()) {
    case vp::Json::Type::Null: return "null";
    case vp::Json::Type::Bool: return value.boolean() ? "true" : "false";
    case vp::Json::Type::Number: { std::ostringstream out; out << std::setprecision(17) << value.num(); return out.str(); }
    case vp::Json::Type::String: return vp::Json::Quote(value.str());
    case vp::Json::Type::Array: {
        std::string out = "[";
        for (std::size_t i = 0; i < value.items().size(); ++i) out += std::string(i ? "," : "") + JsonText(value.items()[i]);
        return out + "]";
    }
    case vp::Json::Type::Object: {
        std::string out = "{";
        for (std::size_t i = 0; i < value.members().size(); ++i)
            out += std::string(i ? "," : "") + vp::Json::Quote(value.members()[i].first) + ":" + JsonText(value.members()[i].second);
        return out + "}";
    }
    }
    return "null";
}

std::string MergeProgressText(const std::string& current_text, const std::string& incoming_text) {
    bool valid = false;
    const vp::Json incoming = vp::Json::Parse(incoming_text, &valid);
    if (!valid || !incoming.is_object()) return {};
    const vp::Json current = vp::Json::Parse(current_text);
    std::map<std::string, vp::Json> merged;
    if (current.is_object()) for (const auto& entry : current.members()) merged[entry.first] = entry.second;
    for (const auto& entry : incoming.members()) {
        const auto old = merged.find(entry.first);
        const auto old_time = old == merged.end() ? std::int64_t(-1) : old->second["updated"].i64(-1);
        const auto new_time = entry.second["updated"].i64(0);
        if (old == merged.end() || new_time >= old_time) merged[entry.first] = entry.second;
    }
    std::string out = "{";
    bool first = true;
    for (const auto& entry : merged) {
        out += std::string(first ? "" : ",") + vp::Json::Quote(entry.first) + ":" + JsonText(entry.second);
        first = false;
    }
    return out + "}\n";
}

bool SaveLanUsers(const std::string& users_text, const std::string& assignments_text) {
    bool valid = false;
    const vp::Json input = vp::Json::Parse(users_text, &valid);
    if (!valid || !input.is_array() || input.items().size() > 128) return false;
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    std::set<std::string> library_ids;
    for (const vp::Json& library : config["libraries"].items()) if (SafeLanUserId(library["id"].str())) library_ids.insert(library["id"].str());
    std::vector<LanUserProfile> users;
    std::set<std::string> user_ids;
    for (const vp::Json& value : input.items()) {
        LanUserProfile user;
        user.id = value["id"].str();
        user.name = value["name"].str();
        user.can_delete = value["canDelete"].boolean();
        if (!SafeLanUserId(user.id) || user.name.empty() || user.name.size() > 64 || !user_ids.insert(user.id).second) return false;
        for (const vp::Json& lib : value["libraries"].items()) {
            const std::string id = lib.str();
            if (!library_ids.count(id)) return false;
            if (std::find(user.libraries.begin(), user.libraries.end(), id) == user.libraries.end()) user.libraries.push_back(id);
        }
        users.push_back(std::move(user));
    }
    if (!user_ids.count("default")) {
        std::vector<std::string> all_libraries(library_ids.begin(), library_ids.end());
        users.insert(users.begin(), LanUserProfile{"default", "一般使用者", std::move(all_libraries), false});
        user_ids.insert("default");
    }
    std::string out = "{\"users\":[";
    for (std::size_t i = 0; i < users.size(); ++i) {
        const auto& user = users[i];
        out += std::string(i ? "," : "") + "{\"id\":" + vp::Json::Quote(user.id) + ",\"name\":" + vp::Json::Quote(user.name) + ",\"canDelete\":" +
               (user.can_delete ? "true" : "false") + ",\"libraries\":[";
        for (std::size_t j = 0; j < user.libraries.size(); ++j) out += std::string(j ? "," : "") + vp::Json::Quote(user.libraries[j]);
        out += "]}";
    }
    out += "]}\n";
    if (!WriteFileUtf8(DataDir() / L"lan_users.json", out)) return false;

    const vp::Json assignments = vp::Json::Parse(assignments_text, &valid);
    if (valid && assignments.is_array()) {
        for (const vp::Json& item : assignments.items()) {
            const std::string client = item["clientId"].str();
            const std::string selected = item["userId"].str();
            const std::string user_id = user_ids.count(selected) ? selected : "default";
            if (client.size() >= 8) g_lan.AssignUser(client, user_id);
        }
    }
    g_lan.RefreshSessions();
    return true;
}

std::string LanLibrariesJson() {
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    std::string out = "[";
    bool first = true;
    for (const vp::Json& library : config["libraries"].items()) {
        out += std::string(first ? "" : ",") + "{\"id\":" + vp::Json::Quote(library["id"].str()) + ",\"name\":" + vp::Json::Quote(library["name"].str()) + "}";
        first = false;
    }
    return out + "]";
}

std::string LanConfigJson(const std::string& user_id) {
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    std::string out = "{\"lanClient\":true,\"libraries\":[";
    bool first = true;
    for (const vp::Json& library : config["libraries"].items()) {
        if (!LanUserCanReadLibrary(user_id, library["id"].str())) continue;
        out += std::string(first ? "" : ",") + "{\"id\":" + vp::Json::Quote(library["id"].str()) + ",\"name\":" + vp::Json::Quote(library["name"].str()) +
               ",\"kind\":" + vp::Json::Quote(library["kind"].str()) + ",\"folders\":[";
        bool first_folder = true;
        for (const vp::Json& folder : library["folders"].items()) {
            out += std::string(first_folder ? "" : ",") + vp::Json::Quote(folder.str());
            first_folder = false;
        }
        out += "]}";
        first = false;
    }
    return out + "]}";
}

// ---------------------------------------------------------------- data-file safety (1B-b H2/H4/H5)
// One "[data]" line in mpv-host.log (UI thread). LAN threads log to lan.log (AppendLanLog) instead.
void DataSafetyLog(const std::string& line) {
#if defined(VP_ENGINE_MPV)
    if (g.player) g.player->HostLog("[data] " + line);
    else AppendHostLogDirect("[data] " + line);
#else
    (void)line;
#endif
}

// A data file that must hold one JSON object (progress.json, lan_progress\<user>.json). state: "ok" (a JSON object),
// "missing" (no file: a first run), "error" (there but unreadable), "empty" (zero bytes or only white space) or
// "invalid" (not a complete JSON object: truncated, corrupt, or another JSON type). Only "ok" and "missing" may be
// written over; text is what was read (BOM removed), also for "invalid".
struct DataFileCheck {
    const char* state = "error";
    std::string text;
};

bool DataStateWritable(const char* state) { return std::strcmp(state, "ok") == 0 || std::strcmp(state, "missing") == 0; }

DataFileCheck CheckJsonObjectFile(const fs::path& path) {
    DataFileCheck check;
    check.text = ReadFileUtf8Status(path, &check.state);
    if (std::strcmp(check.state, "ok") != 0) return check;
    if (check.text.find_first_not_of(" \t\r\n") == std::string::npos) check.state = "empty";
    else if (!IsJsonObjectText(check.text)) check.state = "invalid";
    return check;
}

// 1B-b H4: progress.json fails closed. When the page's last loadData('progress') (or, before any load, the first save)
// found the file unreadable, empty or not a JSON object, the page holds an empty or partial history in memory, and its
// next save would replace the whole watch history. Saves of "progress" are then refused (dataSaved reason "read-only")
// until a later loadData finds the file good again (a page reload after the file was repaired or restored). UI thread.
enum class DataGuard { Unknown, Writable, ReadOnly };
DataGuard g_progress_guard = DataGuard::Unknown;
std::string g_progress_guard_state;
bool g_progress_refusal_logged = false;

void SetProgressGuard(const char* state, const char* why) {
    const bool read_only = !DataStateWritable(state);
    const DataGuard next = read_only ? DataGuard::ReadOnly : DataGuard::Writable;
    if (read_only && (g_progress_guard != DataGuard::ReadOnly || g_progress_guard_state != state))
        DataSafetyLog(std::string("progress.json is ") + state + " (" + why + "): progress is read-only for this session; the file is left as it is");
    else if (!read_only && g_progress_guard == DataGuard::ReadOnly)
        DataSafetyLog(std::string("progress.json is ") + state + " again (" + why + "): progress saves allowed");
    g_progress_guard = next;
    g_progress_guard_state = state;
    g_progress_refusal_logged = false;
}

bool ProgressWritable() {
    if (g_progress_guard == DataGuard::Unknown) SetProgressGuard(CheckJsonObjectFile(DataDir() / L"progress.json").state, "checked before the first save");
    if (g_progress_guard != DataGuard::ReadOnly) return true;
    if (!g_progress_refusal_logged) {
        DataSafetyLog("saveData progress refused: read-only (progress.json was " + g_progress_guard_state + " when loaded); later refusals are not logged");
        g_progress_refusal_logged = true;
    }
    return false;
}

// 1B-b H5: anilist.json / prefs.json keep three earlier generations (.bak1 newest .. .bak3), rotated on the first
// replacement of each key in a session and then at most every 30 minutes. The page saves anilist ~800 ms after every
// change and prefs at once, so rotating on every save would make all three generations seconds apart and one bad
// session could push every good copy out. UI thread.
std::map<std::string, std::chrono::steady_clock::time_point> g_backup_rotated;

bool BackupRotationDue(const std::string& key) {
    const auto it = g_backup_rotated.find(key);
    return it == g_backup_rotated.end() || std::chrono::steady_clock::now() - it->second >= std::chrono::minutes(30);
}

void AppendLanLog(const std::string& line);

// Read-only data for LAN clients: metadata, preferences without credentials, the server's progress.
std::string LanData(const std::string& user_id, const std::string& key) {
    if (key != "anilist" && key != "prefs" && key != "progress") return {};
    std::string text;
    if (key == "progress") {
        if (!SafeLanUserId(user_id)) return "{}";
        text = ReadFileUtf8(DataDir() / L"lan_progress" / (Wide(user_id) + L".json"));
        if (text.empty()) text = "{}";
    } else {
        text = ReadFileUtf8(DataDir() / (Wide(key) + L".json"));
    }
    if (key == "prefs") text = BlankJsonString(BlankJsonString(std::move(text), "tmdbKey"), "bgmToken");
    return LanRewrite(std::move(text));
}

// 1B-b H4: the stored file is merged with the incoming progress, so it must be readable first. An unreadable, empty or
// corrupt lan_progress\<user>.json is never replaced by the incoming entries alone: the save is refused (LAN POST
// /api/data answers 400 {"ok":false}) and lan.log says so once per user and state. Checked on every save, so a
// repaired file is merged again at once (the merge always starts from the file, not from a page's memory).
bool SaveLanProgress(const std::string& user_id, const std::string& text) {
    std::lock_guard<std::mutex> lock(LanProgressMutex());
    if (!SafeLanUserId(user_id) || text.size() > 16 * 1024 * 1024) return false;
    if (!IsJsonObjectText(text)) return false;
    const fs::path path = DataDir() / L"lan_progress" / (Wide(user_id) + L".json");
    const DataFileCheck current = CheckJsonObjectFile(path);
    static std::set<std::string> refused_logged; // guarded by LanProgressMutex
    if (!DataStateWritable(current.state)) {
        if (refused_logged.insert(user_id + "|" + current.state).second)
            AppendLanLog("progress not saved for user " + user_id + ": lan_progress\\" + user_id + ".json is " + current.state +
                         "; the file is left as it is (repair or restore it; later refusals are not logged)");
        return false;
    }
    refused_logged.erase(user_id + "|error"); refused_logged.erase(user_id + "|empty"); refused_logged.erase(user_id + "|invalid");
    const std::string merged = MergeProgressText(current.text, text);
    return !merged.empty() && WriteFileUtf8(path, merged);
}

// Server mode: the desktop page's progress is progress.json merged with lan_progress\default.json, written back to
// both. 1B-b H4: nothing is written unless both files are good (or missing); the reply state is progress.json's.
DataFileCheck LoadDefaultLanProgress() {
    std::lock_guard<std::mutex> lock(LanProgressMutex());
    const fs::path local_path = DataDir() / L"progress.json";
    const fs::path shared_path = DataDir() / L"lan_progress" / L"default.json";
    DataFileCheck local = CheckJsonObjectFile(local_path);
    if (!DataStateWritable(local.state)) return local;
    const DataFileCheck shared = CheckJsonObjectFile(shared_path);
    if (std::strcmp(shared.state, "ok") != 0) {
        if (!DataStateWritable(shared.state)) AppendLanLog(std::string("lan_progress\\default.json is ") + shared.state + ": not merged into progress.json; the file is left as it is");
        return local;
    }
    const std::string merged = MergeProgressText(local.text, shared.text);
    if (merged.empty()) return local;
    WriteFileUtf8(local_path, merged);
    WriteFileUtf8(shared_path, merged);
    return DataFileCheck{"ok", merged};
}

std::string ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? Utf8(name) : std::string("SceneArc");
}

void AppendLanLog(const std::string& line) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::error_code ec;
    fs::create_directories(DataDir() / L"logs", ec);
    std::ofstream log(DataDir() / L"logs" / L"lan.log", std::ios::app | std::ios::binary);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    log << stamp << line << "\r\n";
}

// RPC from a LAN client: only commands that read the library or generate cached images.
void HandleLanRpc(std::uint64_t session, const std::string& user_id, const std::string& text) {
    bool ok = false;
    const vp::Json probe = vp::Json::Parse(text, &ok);
    if (!ok || !probe.is_object() || text.empty() || text[0] != '{') return;
    const std::string cmd = probe["cmd"].str();
    const vp::Json message = vp::Json::Parse("{\"__lan\":" + std::to_string(session) + ",\"__user\":" + vp::Json::Quote(user_id) + (text.size() > 2 ? "," : "") + text.substr(1));
    if (cmd == "scan") ScanLibraries(message);
    else if (cmd == "thumbnails") StartThumbnails(message);
    else if (cmd == "thumbnailsAt") StartThumbnailsAt(message);
    else if (cmd == "cancelThumbnails") ++ThumbCounter(session, SlotIndex(message["slot"].str()));
    else if (cmd == "artwork") ExtractArtwork(message);
    else if (cmd == "getConfig") g_lan.Push(session, Reply("config", message, "\"text\":" + vp::Json::Quote(LanConfigJson(user_id))));
    else if (cmd == "loadData") {
        const std::string key = message["key"].str();
        g_lan.Push(session, Reply("data", message, "\"key\":" + vp::Json::Quote(key) + ",\"text\":" + vp::Json::Quote(LanData(user_id, key))));
    } else if (message["reqId"].type() == vp::Json::Type::Number) {
        g_lan.Push(session, Reply("denied", message, "\"cmd\":" + vp::Json::Quote(cmd))); // all unlisted client commands are refused
    }
}

std::string LanStatusJson() {
    EnsureLanUsersFile();
    std::string status = g_lan.running() ? g_lan.StatusJson() : std::string("{\"running\":false}");
    return "\"mode\":" + vp::Json::Quote(g_lan_mode) + ",\"server\":" + vp::Json::Quote(g_lan_server) + ",\"status\":" + status +
           ",\"users\":" + LanUsersJson() + ",\"libraries\":" + LanLibrariesJson();
}

// (Re)reads config.json "lan" and starts or stops sharing.
#if !defined(VP_ENGINE_MPV)
void ApplyRifeModelPath(const vp::Json& config);
#endif

// 1.0.0: 區網分享 is not offered yet. The saved "lan" section is kept untouched, but the player always runs
// standalone: no HTTP or discovery port is opened and client mode never loads a server's page.
constexpr bool kLanSharingAvailable = false;

void ApplyLanConfig() {
    if (!kLanSharingAvailable) {
        g_lan_mode = "standalone";
        g_lan_server.clear();
        g_lan.Stop();
        return;
    }
    const vp::Json config = vp::Json::Parse(LoadConfigText());
    const vp::Json& lan = config["lan"];
    g_lan_mode = lan["mode"].str("standalone");
    g_lan_server = lan["server"].str();
    while (!g_lan_server.empty() && g_lan_server.back() == '/') g_lan_server.pop_back();
    if (g_lan_mode != "server") {
        g_lan.Stop();
        return;
    }
    const auto port = static_cast<std::uint16_t>(lan["port"].i64(vp::lan::kDefaultHttpPort));
    if (g_lan.running() && g_lan.port() == port) return;
    EnsureLanUsersFile();
    vp::lan::LanHooks hooks;
    hooks.ui_dir = g.root / L"ui";
    hooks.data_dir = DataDir();
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    hooks.fonts_dir = fs::path(windows) / L"Fonts";
    hooks.server_name = lan["name"].str(ComputerName());
#if defined(VP_ENGINE_MPV)
    // The MPV edition has no PlaybackCore, so server-processed (super-resolution) streams are unavailable.
    hooks.stream_mode = [] { return std::string("original"); };
#else
    hooks.stream_mode = [] {
        const vp::Json current = vp::Json::Parse(LoadConfigText());
        return current["lan"]["streamMode"].str("original") == "superres" ? std::string("superres") : std::string("original");
    };
#endif
    // Access control, config, data and RPC hooks are engine-independent and needed by both editions.
    hooks.can_access_file = [](const std::string& user_id, const fs::path& file) { return LanUserCanReadPath(user_id, file); };
    hooks.can_access_thumbnail = [](const std::string& user_id, const std::string& relative_path) { return LanUserCanReadThumbnail(user_id, relative_path); };
    hooks.can_delete_media = [](const std::string& user_id) { return LanUserCanDelete(user_id); };
    hooks.user_libraries = [](const std::string& user_id) { return LanUserLibrariesJson(user_id); };
    hooks.read_config = [](const std::string& user_id) { return LanConfigJson(user_id); };
    hooks.read_data = [](const std::string& user_id, const std::string& key) { return LanData(user_id, key); };
    hooks.write_data = [](const std::string& user_id, const std::string& key, const std::string& text) {
        return key == "progress" && SaveLanProgress(user_id, text);
    };
    hooks.rpc = [](std::uint64_t session, const std::string& user_id, const std::string& json) {
        auto* payload = new LanMessage{0, session, json, user_id};
        if (!PostMessageW(g.window, WM_APP_LAN, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    };
    hooks.delete_media = [](const std::string& user_id, const fs::path& file) {
        if (!LanUserCanDelete(user_id) || !LanUserCanReadPath(user_id, file)) return false;
        std::wstring source = file.wstring();
        source.push_back(L'\0');
        source.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = source.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
    };
    hooks.log = [](const std::string& line) { AppendLanLog(line); };
#if defined(VP_ENGINE_MPV)
    vp::remux::InstallLanHooks(hooks, DataDir() / L"cache");
#elif !defined(VP_NO_REMUX)
    vp::remux::InstallLanHooks(hooks, DataDir() / L"cache", [] {
        const vp::Json current = vp::Json::Parse(LoadConfigText());
        ApplyRifeModelPath(current);
        const vp::Json& value = current["upscale"];
        const std::string mode = value["mode"].str();
        const std::string model = value["animejanaiModel"].str("v2_performance");
        std::int64_t height = value["inferenceHeights"][model].i64(0);
        if (height != 720 && height != 1080) height = value["inferenceHeight"].i64(720);
        if (height != 720 && height != 1080) height = 720;
        const std::int32_t upscale_mode = mode == "anime4k" ? PC_UPSCALE_ANIME4K_ACNET
            : mode == "animejanai_l1" ? PC_UPSCALE_ANIMEJANAI_V2_L1 : PC_UPSCALE_OFF;
        const std::int32_t animejanai_model = model == "v3_balanced" ? PC_ANIMEJANAI_HD_V3_BALANCED
            : model == "v2_balanced" ? PC_ANIMEJANAI_V2_BALANCED
            : model == "v3_performance" ? PC_ANIMEJANAI_HD_V3_PERFORMANCE : PC_ANIMEJANAI_V2_PERFORMANCE;
        const std::int32_t interpolation_backend = current["frameInterpolationBackend"].str("amf") == "rife"
            ? PC_FRAME_INTERPOLATION_RIFE : PC_FRAME_INTERPOLATION_AMF;
        const vp::Json restore = current["restore"];
        return vp::remux::StreamProcessingSettings{
            upscale_mode,
            animejanai_model,
            static_cast<std::int32_t>(height),
            interpolation_backend,
            restore["enabled"].boolean(),
            restore["model"].str("Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx"),
            current["rifeOutputWarp"].boolean(),
            current["frcDedup"].boolean()
        };
    });
#endif
    std::string error;
    if (!g_lan.Start(port, std::move(hooks), &error)) AppendLanLog("LAN server failed to start: " + error);
}

// ---------------------------------------------------------------- LAN sharing: client side
bool StartsWith(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

// "http://host:port" of a server address the user typed or discovery found; empty when invalid.
std::string NormalizeServer(std::string address) {
    while (!address.empty() && (address.back() == '/' || address.back() == ' ')) address.pop_back();
    while (!address.empty() && address.front() == ' ') address.erase(0, 1);
    if (address.empty()) return {};
    if (!StartsWith(address, "http://")) address = "http://" + address;
    const std::string host = address.substr(7);
    if (host.empty() || host.find_first_of("/?#@ ") != std::string::npos) return {};
    if (host.find(':') == std::string::npos) address += ":" + std::to_string(vp::lan::kDefaultHttpPort);
    return address;
}

// Downloads a side-car subtitle from the server (the core reads text subtitles from disk).
void DownloadSubtitle(const std::string& url) {
    std::thread([url]() {
        URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
        wchar_t host[256] = {}, path[4096] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        wchar_t extra[4096] = {};
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
        const std::wstring wide = Wide(url);
        if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return;
        std::string body;
        HINTERNET session = WinHttpOpen(L"VideoPlayer/1.0 (LAN client)", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
        const std::wstring object = std::wstring(path) + extra;
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        DWORD code = 0, size = sizeof(code);
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) &&
            code == 200) {
            for (;;) {
                DWORD available = 0, read = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
                std::string chunk(available, '\0');
                if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
                body.append(chunk.data(), read);
                if (body.size() > 64 * 1024 * 1024) break;
            }
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
        if (body.empty()) return;
        // Keep the original extension: the core picks the subtitle parser from it.
        const std::string original = vp::lan::PercentDecode(url.substr(url.find("p=") == std::string::npos ? url.size() : url.find("p=") + 2));
        const fs::path name = vp::lan::PathFromUtf8(original.substr(0, original.find('&'))).filename();
        const fs::path dir = DataDir() / L"cache" / L"lan-subtitles";
        std::error_code ec;
        fs::create_directories(dir, ec);
        char key[32];
        std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(std::hash<std::string>{}(url)));
        const fs::path file = dir / (Wide(key) + L"_" + name.wstring());
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        }
        auto* payload = new LanMessage{1, 0, Utf8(file.wstring())};
        if (!PostMessageW(g.window, WM_APP_LAN, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    }).detach();
}

void NavigateToUi() {
    if (!g.webview) return;
    if (g_lan_mode == "client" && !g_lan_server.empty()) g.webview->Navigate(Wide(g_lan_server + "/ui/index.html?native=1").c_str());
    else g.webview->Navigate(g_lan_mode == "client" ? L"https://app.local/index.html?connect=1" : L"https://app.local/index.html");
}

// Commands from the server's page in client mode: playback only, and only of that server's streams.
void HandleServerPageMessage(const std::string& text) {
    bool ok = false;
    const vp::Json message = vp::Json::Parse(text, &ok);
    if (!ok) return;
    const std::string cmd = message["cmd"].str();
    const std::string streams = g_lan_server + "/stream/";
    if (cmd == "open") {
        const std::string url = message["url"].str();
        if (!StartsWith(url, streams)) return;
        EnsurePlayer();
#if defined(VP_ENGINE_MPV)
        if (!g.player || !g.player->Ready()) {
            const std::string code = g.player_error_code.empty() ? "MPV_INIT" : g.player_error_code;
            const int result = code == "LIBMPV_MISSING" ? -100 : -1;
            PostToWeb("{\"type\":\"player\",\"event\":\"error\",\"data\":{\"fatal\":true,\"code\":" +
                       vp::Json::Quote(code) + ",\"message\":" + vp::Json::Quote(g.player_error) + "}}");
            PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(result) + ",\"code\":" + vp::Json::Quote(code) + ",\"engine\":\"mpv\""));
            return;
        }
        const int result = g.player->Open(url, message["startMs"].i64(), message["paused"].boolean(), message["noAudio"].boolean());
        PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(result) + ",\"engine\":\"mpv\""));
#else
        // This PC only decodes and presents: no frame interpolation or upscaling on a LAN client.
        int32_t interpolation_result = pc_set_frame_interpolation(g.player, 0, nullptr);
        if (interpolation_result == PC_E_STATE) {
            pc_stop(g.player);
            interpolation_result = pc_set_frame_interpolation(g.player, 0, nullptr);
        }
        pc_set_rife_restore(g.player, 0, nullptr);
        pc_set_upscale(g.player, PC_UPSCALE_OFF);
        uint32_t flags = 0;
        if (message["paused"].boolean()) flags |= PC_OPEN_PAUSED;
        if (message["noAudio"].boolean()) flags |= PC_OPEN_NO_AUDIO;
        const int32_t result = pc_open(g.player, url.c_str(), message["startMs"].i64(), flags);
        PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(result) + ",\"frameInterpolationResult\":" + std::to_string(interpolation_result)));
#endif
    } else if (cmd == "loadSubtitle") {
        const std::string url = message["url"].str();
        if (StartsWith(url, streams + "sub?")) DownloadSubtitle(url);
    } else if (cmd == "play" || cmd == "pause" || cmd == "toggle" || cmd == "seek" || cmd == "seekRelative" || cmd == "selectSubtitle" ||
               cmd == "setVolume" || cmd == "setMute" ||
               cmd == "stop" || cmd == "fullscreen" || cmd == "quit" || cmd == "lanDisconnect" || cmd == "lanInfo") {
        HandleWebMessage(text); // the same handlers as the local page
        if (cmd == "stop") ReleaseVideoSurface(); // that page has no releaseSurface handshake
    }
}

// RIFE model selection applies when the plugin creates its session, hence takes
// effect on the next opened video. An externally set PC_RIFE_MODEL_PATH (for
// diagnostics) is intentionally left untouched.
#if !defined(VP_ENGINE_MPV)
void ApplyRifeModelPath(const vp::Json& config) {
    static const bool external_override = [] {
        wchar_t probe[8]{};
        return GetEnvironmentVariableW(L"PC_RIFE_MODEL_PATH", probe, static_cast<DWORD>(std::size(probe))) > 0;
    }();
    if (external_override) return;
    const fs::path fp16_model = ExeDir() / L"models" / L"rife_v4.6_fp16.onnx";
    const fs::path lite_model = ExeDir() / L"models" / L"rife_v4.25_lite.onnx";
    std::error_code ec;
    // The published v4.25 Lite graph is FP32. A configured model continues to
    // use its native source dimensions; do not silently lower its resolution.
    if (config["rifeModel"].str("v46") == "v425_lite" && fs::exists(lite_model, ec)) {
        SetEnvironmentVariableW(L"PC_RIFE_MODEL_PATH", lite_model.c_str());
        SetEnvironmentVariableW(L"PC_RIFE_INFERENCE_HEIGHT", nullptr);
        return;
    }
    SetEnvironmentVariableW(L"PC_RIFE_INFERENCE_HEIGHT", nullptr);
    if (config["rifePrecision"].str("fp32") == "fp16" && fs::exists(fp16_model, ec))
        SetEnvironmentVariableW(L"PC_RIFE_MODEL_PATH", fp16_model.c_str());
    else
        SetEnvironmentVariableW(L"PC_RIFE_MODEL_PATH", nullptr); // plugin default: models\rife_v4.6.onnx (fp32)
}

// x1 restoration deliberately runs inside the RIFE DirectML pipeline.  It is
// selected once per opened title; the core rejects a model path rather than a
// portable filename in the local models folder.
bool RifeRestoreRequested(const vp::Json& config, bool interpolation, int32_t backend, std::string* model_out) {
    const vp::Json restore = config["restore"];
    const bool enabled = interpolation && backend == PC_FRAME_INTERPOLATION_RIFE && restore["enabled"].boolean();
    const std::string model = restore["model"].str("Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx");
    if (model_out) *model_out = model;
    return enabled;
}
#endif

#if defined(VP_ENGINE_MPV)
// ---------------------------------------------------------------- diagnostics export (exportDiagnostics)
// Settings 處理環境 and the player's info panel 「匯出診斷資料」: one zip under data\diagnostics\ with what a tester is
// asked to send (05-nvidia-validation-procedure.md §5: logs, generated .vpy, crash dumps, trtexec logs) plus
// system.txt / runtime.txt / state.json / config-video.json written here. The UI thread only snapshots the backend
// state and the config; reading files, hashing, WMI and writing the zip run on a worker thread.
//
// Zip format: a plain ZIP with every entry STORED (no compression), written by the small writer below. The host links
// no zlib (FFmpeg's DLLs do not export it), and the Shell's Folder.CopyHere zip path is asynchronous with no completion
// signal and can show UI. A stored ZIP is synchronous, has no dependency, and opens in Explorer, 7-Zip and
// Expand-Archive. Inputs are capped (5 MB per log, 50 MB of crash dumps), so the file stays far below ZIP's 4 GB limit.
namespace diag {

constexpr std::uint64_t kLogTailBytes = 5ull * 1024 * 1024;
constexpr std::uint64_t kCrashBudgetBytes = 50ull * 1024 * 1024;
constexpr std::size_t kTrtexecLogs = 5;
constexpr std::size_t kVpyMax = 200;

std::uint32_t Crc32(const std::string& data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char ch : data) crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void Put16(std::string& out, std::uint32_t v) { out.push_back(static_cast<char>(v & 0xFF)); out.push_back(static_cast<char>((v >> 8) & 0xFF)); }
void Put32(std::string& out, std::uint32_t v) { Put16(out, v & 0xFFFF); Put16(out, v >> 16); }

class ZipWriter {
public:
    explicit ZipWriter(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
    bool ok() const { return static_cast<bool>(out_) && !failed_; }
    std::uint64_t bytes() const { return offset_; }
    // name: forward slashes, UTF-8 (general-purpose flag bit 11). modified: UTF FILETIME, nullptr = now.
    bool Add(const std::string& name, const std::string& data, const FILETIME* modified) {
        if (!ok()) return false;
        if (entries_.size() >= 0xFFFF || name.size() > 0xFFFF || offset_ + 30 + name.size() + data.size() >= 0xFFFFFFFFull) {
            failed_ = true;
            return false;
        }
        Entry entry{name, Crc32(data), static_cast<std::uint32_t>(data.size()), static_cast<std::uint32_t>(offset_), 0, 0};
        DosTime(modified, &entry.time, &entry.date);
        std::string header;
        Put32(header, 0x04034b50);  // local file header
        Put16(header, 10);          // version needed: 1.0 (stored)
        Put16(header, 0x0800);      // UTF-8 names
        Put16(header, 0);           // method: stored
        Put16(header, entry.time);
        Put16(header, entry.date);
        Put32(header, entry.crc);
        Put32(header, entry.size);  // compressed size
        Put32(header, entry.size);  // uncompressed size
        Put16(header, static_cast<std::uint32_t>(name.size()));
        Put16(header, 0);           // extra length
        header += name;
        out_.write(header.data(), static_cast<std::streamsize>(header.size()));
        out_.write(data.data(), static_cast<std::streamsize>(data.size()));
        offset_ += header.size() + data.size();
        entries_.push_back(std::move(entry));
        return ok();
    }
    bool Finish() {
        if (!ok()) return false;
        const std::uint64_t directory_at = offset_;
        std::string directory;
        for (const Entry& entry : entries_) {
            Put32(directory, 0x02014b50); // central directory header
            Put16(directory, 20);         // made by: MS-DOS, 2.0
            Put16(directory, 10);
            Put16(directory, 0x0800);
            Put16(directory, 0);
            Put16(directory, entry.time);
            Put16(directory, entry.date);
            Put32(directory, entry.crc);
            Put32(directory, entry.size);
            Put32(directory, entry.size);
            Put16(directory, static_cast<std::uint32_t>(entry.name.size()));
            Put16(directory, 0);          // extra
            Put16(directory, 0);          // comment
            Put16(directory, 0);          // disk
            Put16(directory, 0);          // internal attributes
            Put32(directory, 0);          // external attributes
            Put32(directory, entry.offset);
            directory += entry.name;
        }
        if (directory_at + directory.size() + 22 >= 0xFFFFFFFFull) return false;
        std::string end;
        Put32(end, 0x06054b50);
        Put16(end, 0);
        Put16(end, 0);
        Put16(end, static_cast<std::uint32_t>(entries_.size()));
        Put16(end, static_cast<std::uint32_t>(entries_.size()));
        Put32(end, static_cast<std::uint32_t>(directory.size()));
        Put32(end, static_cast<std::uint32_t>(directory_at));
        Put16(end, 0);
        out_.write(directory.data(), static_cast<std::streamsize>(directory.size()));
        out_.write(end.data(), static_cast<std::streamsize>(end.size()));
        offset_ += directory.size() + end.size();
        out_.flush();
        const bool written = ok();
        out_.close();
        return written && !out_.fail();
    }

private:
    struct Entry {
        std::string name;
        std::uint32_t crc{}, size{}, offset{};
        std::uint16_t time{}, date{};
    };
    static void DosTime(const FILETIME* modified, std::uint16_t* time, std::uint16_t* date) {
        FILETIME utc{};
        if (modified && (modified->dwLowDateTime || modified->dwHighDateTime)) utc = *modified;
        else GetSystemTimeAsFileTime(&utc);
        FILETIME local{};
        WORD dos_date = 0, dos_time = 0;
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToDosDateTime(&local, &dos_date, &dos_time)) { dos_date = 0x21; dos_time = 0; }
        *time = dos_time;
        *date = dos_date;
    }
    std::ofstream out_;
    std::vector<Entry> entries_;
    std::uint64_t offset_{};
    bool failed_{};
};

struct FileRead {
    bool ok{};
    std::string data;
    std::uint64_t size{};  // size on disk
    bool truncated{};      // only the tail was read
    FILETIME modified{};
    DWORD error{};
};

// Opens with every share flag, so logs that mpv / the log writer keep open (and append to) can be read.
FileRead ReadShared(const fs::path& path, std::uint64_t max_bytes, bool cut_to_line) {
    FileRead r;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { r.error = GetLastError(); return r; }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    r.size = static_cast<std::uint64_t>(size.QuadPart);
    GetFileTime(file, nullptr, nullptr, &r.modified);
    std::uint64_t start = 0;
    if (max_bytes && r.size > max_bytes) { start = r.size - max_bytes; r.truncated = true; }
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(start);
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    r.data.resize(static_cast<std::size_t>(r.size - start));
    std::size_t got = 0;
    while (got < r.data.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(r.data.size() - got, 1u << 20));
        if (!ReadFile(file, r.data.data() + got, want, &n, nullptr) || n == 0) break;
        got += n;
    }
    r.data.resize(got);
    CloseHandle(file);
    if (r.truncated && cut_to_line) {
        const std::size_t newline = r.data.find('\n');
        if (newline != std::string::npos && newline < 65536) r.data.erase(0, newline + 1);
    }
    r.ok = true;
    return r;
}

bool FileAttributes(const fs::path& path, std::uint64_t* size, FILETIME* modified) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (size) *size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (modified) *modified = data.ftLastWriteTime;
    return true;
}

std::string FormatFileTime(const FILETIME& utc) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return "?";
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return text;
}

std::string NowText(bool compact) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char text[32]{};
    if (compact) std::snprintf(text, sizeof(text), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return text;
}

std::string Hex(std::uint64_t value, int width) {
    char text[24]{};
    std::snprintf(text, sizeof(text), "%0*llX", width, static_cast<unsigned long long>(value));
    return text;
}

std::string Mib(std::uint64_t bytes) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

std::string Sha256File(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
    std::string out;
    BCRYPT_HASH_HANDLE hash{};
    if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buffer(1u << 20);
            bool ok = true;
            DWORD n = 0;
            while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) && n)
                if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), n, 0))) { ok = false; break; }
            CloseHandle(file);
            unsigned char digest[32]{};
            if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)))
                for (const unsigned char b : digest) out += Hex(b, 2);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

// vs-mlrt release tags embedded in vstrt.dll / vsort.dll, e.g. "v15.16-0-g885e8bb" (git describe). The validation script
// checks the same string with findstr. Bare "vNN.NN" hits are reported only when no full tag is found.
std::vector<std::string> FindReleaseTags(const std::string& bytes) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    std::set<std::string> full, bare;
    for (std::size_t at = bytes.find('v'); at != std::string::npos && full.size() < 8; at = bytes.find('v', at + 1)) {
        if (at > 0 && std::isalnum(static_cast<unsigned char>(bytes[at - 1]))) continue;
        std::size_t p = at + 1;
        const std::size_t major_at = p;
        while (p < bytes.size() && digit(bytes[p])) ++p;
        if (p - major_at != 2 || bytes.compare(major_at, 1, "1") != 0) continue; // v10..v19
        if (p >= bytes.size() || bytes[p] != '.') continue;
        const std::size_t minor_at = ++p;
        while (p < bytes.size() && digit(bytes[p])) ++p;
        if (p == minor_at || p - minor_at > 3) continue;
        const std::string version = bytes.substr(at, p - at);
        // optional -<commits>-g<hash>
        std::size_t q = p;
        if (q < bytes.size() && bytes[q] == '-') {
            const std::size_t commits_at = ++q;
            while (q < bytes.size() && digit(bytes[q])) ++q;
            if (q > commits_at && q + 1 < bytes.size() && bytes[q] == '-' && bytes[q + 1] == 'g') {
                const std::size_t hash_at = q + 2;
                std::size_t h = hash_at;
                while (h < bytes.size() && hex(bytes[h])) ++h;
                if (h - hash_at >= 7) { full.insert(bytes.substr(at, h - at)); continue; }
            }
        }
        if (bare.size() < 8 && (p >= bytes.size() || !std::isalnum(static_cast<unsigned char>(bytes[p])))) bare.insert(version);
    }
    const std::set<std::string>& chosen = full.empty() ? bare : full;
    return std::vector<std::string>(chosen.begin(), chosen.end());
}

std::string RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS) return {};
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return Utf8(text);
}

bool RegNumber(HKEY root, const wchar_t* key, const wchar_t* value, std::uint64_t* out) {
    std::uint64_t number = 0;
    DWORD bytes = sizeof(number);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD | RRF_RT_REG_QWORD, nullptr, &number, &bytes) != ERROR_SUCCESS) return false;
    *out = bytes == sizeof(DWORD) ? static_cast<std::uint32_t>(number) : number;
    return true;
}

// WQL query on ROOT\CIMV2 (the calling thread must have COM initialised). Each row maps property -> text.
std::vector<std::map<std::string, std::string>> WmiQuery(const wchar_t* wql, const std::vector<const wchar_t*>& properties, std::string* error) {
    std::vector<std::map<std::string, std::string>> rows;
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (FAILED(hr)) { if (error) *error = "WbemLocator 0x" + Hex(static_cast<std::uint32_t>(hr), 8); return rows; }
    ComPtr<IWbemServices> services;
    BSTR resource = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(resource, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(resource);
    if (FAILED(hr)) { if (error) *error = "ConnectServer 0x" + Hex(static_cast<std::uint32_t>(hr), 8); return rows; }
    CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(wql);
    ComPtr<IEnumWbemClassObject> enumerator;
    hr = services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr)) { if (error) *error = "ExecQuery 0x" + Hex(static_cast<std::uint32_t>(hr), 8); return rows; }
    CoSetProxyBlanket(enumerator.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    for (int guard = 0; guard < 64; ++guard) {
        ComPtr<IWbemClassObject> object;
        ULONG returned = 0;
        hr = enumerator->Next(5000, 1, &object, &returned);
        if (FAILED(hr) || returned == 0 || hr == WBEM_S_TIMEDOUT) {
            if (hr == WBEM_S_TIMEDOUT && error) *error = "timed out after 5 s";
            break;
        }
        std::map<std::string, std::string> row;
        for (const wchar_t* property : properties) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(object->Get(property, 0, &value, nullptr, nullptr)) && value.vt != VT_NULL && value.vt != VT_EMPTY) {
                VARIANT text;
                VariantInit(&text);
                if (SUCCEEDED(VariantChangeType(&text, &value, 0, VT_BSTR)) && text.vt == VT_BSTR && text.bstrVal) row[Utf8(property)] = Utf8(std::wstring(text.bstrVal, SysStringLen(text.bstrVal)));
                VariantClear(&text);
            }
            VariantClear(&value);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string FileVersionText(const fs::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    std::string out;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) && fixed && length) {
        out = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
              std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
    }
    struct Translation { WORD language, codepage; }* translations = nullptr;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) && translations &&
        length >= sizeof(Translation)) {
        wchar_t key[64]{};
        std::swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\ProductVersion", translations[0].language, translations[0].codepage);
        wchar_t* product = nullptr;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&product), &length) && product && length)
            out += (out.empty() ? "" : " (product ") + Utf8(product) + (out.empty() ? "" : ")");
    }
    return out;
}

// Indents compact JSON (strings kept as they are) so the developer can read state.json / config-video.json.
std::string PrettyJson(const std::string& compact) {
    std::string out;
    int depth = 0;
    bool in_string = false, escape = false;
    const auto newline = [&] { out += '\n'; out.append(static_cast<std::size_t>(std::max(depth, 0)) * 2, ' '); };
    for (std::size_t i = 0; i < compact.size(); ++i) {
        const char c = compact[i];
        if (in_string) {
            out += c;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        switch (c) {
        case '"': in_string = true; out += c; break;
        case '{': case '[':
            if (i + 1 < compact.size() && (compact[i + 1] == '}' || compact[i + 1] == ']')) { out += c; out += compact[++i]; break; }
            out += c; ++depth; newline(); break;
        case '}': case ']': --depth; newline(); out += c; break;
        case ',': out += c; newline(); break;
        case ':': out += ": "; break;
        case ' ': case '\n': case '\r': case '\t': break;
        default: out += c;
        }
    }
    return out + "\n";
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

std::string FirstLineContaining(const fs::path& path, const std::string& needle) {
    std::istringstream lines(ReadFileUtf8(path));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(needle) != std::string::npos) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            return line;
        }
    }
    return {};
}

struct Request {
    fs::path exe_dir, data_dir, app_root, zip_path;
    std::string raw_config;
    std::string state_json;
    std::string refresh_summary; // 1B-b H6: the page's 刷新全部中繼資料 summary from the request (capped), or empty
    std::string mpv_version, ffmpeg_version;
    bool player_ready{};
    std::string created;
};

struct Result {
    bool ok{};
    fs::path zip_path;
    std::uint64_t bytes{};
    std::vector<std::string> items, warnings;
    std::string error;
    double ms{};
};

std::string InstallKind(const fs::path& exe_dir, const fs::path& app_root) {
    const std::string dir = Lower(Utf8(exe_dir.wstring()));
    if (dir.find("\\.claude\\worktrees\\") != std::string::npos) return "worktree build (development, git worktree)";
    wchar_t local[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    if (n && n < std::size(local)) {
        const std::string programs = Lower(Utf8(std::wstring(local, n))) + "\\programs\\";
        if (dir.rfind(programs, 0) == 0) return "installed (per-user, %LOCALAPPDATA%\\Programs)";
    }
    if (!app_root.empty() && app_root != exe_dir) return "source-tree build (development; ui\\ found above the exe)";
    return "other location (portable copy or custom install)";
}

std::string SystemText(const Request& req, std::vector<std::string>* warnings) {
    std::ostringstream s;
    s << "SceneArc diagnostics: system\r\nCreated: " << req.created << "\r\n\r\n";
    // Windows
    s << "[Windows]\r\n";
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&os);
    const wchar_t* nt_key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::uint64_t ubr = 0;
    RegNumber(HKEY_LOCAL_MACHINE, nt_key, L"UBR", &ubr);
    s << "Version: " << os.dwMajorVersion << "." << os.dwMinorVersion << "." << os.dwBuildNumber << "." << ubr
      << (os.dwBuildNumber >= 22000 ? " (Windows 11)" : " (Windows 10 or older)") << "\r\n";
    s << "ProductName (registry): " << RegString(HKEY_LOCAL_MACHINE, nt_key, L"ProductName") << "\r\n";
    s << "EditionID: " << RegString(HKEY_LOCAL_MACHINE, nt_key, L"EditionID") << "\r\n";
    s << "DisplayVersion: " << RegString(HKEY_LOCAL_MACHINE, nt_key, L"DisplayVersion") << "\r\n\r\n";
    // CPU / RAM
    s << "[CPU / memory]\r\n";
    s << "CPU: " << RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") << "\r\n";
    s << "Logical processors: " << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) << "\r\n";
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        s << "RAM: total " << Mib(memory.ullTotalPhys) << ", available " << Mib(memory.ullAvailPhys) << " (load " << memory.dwMemoryLoad << "%)\r\n";
    s << "\r\n";
    // GPUs: DXGI (vendor/device id, dedicated VRAM) and WMI Win32_VideoController (driver version).
    s << "[GPU adapters: DXGI]\r\n";
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; index < 16 && SUCCEEDED(factory->EnumAdapters1(index, &adapter)); ++index) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                LARGE_INTEGER umd{};
                const bool has_umd = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                s << "#" << index << " " << Utf8(desc.Description) << ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "")
                  << "\r\n   vendor=0x" << Hex(desc.VendorId, 4) << " device=0x" << Hex(desc.DeviceId, 4) << " subsys=0x" << Hex(desc.SubSysId, 8)
                  << " rev=" << desc.Revision << "\r\n   dedicated VRAM " << Mib(desc.DedicatedVideoMemory) << ", dedicated system "
                  << Mib(desc.DedicatedSystemMemory) << ", shared " << Mib(desc.SharedSystemMemory) << "\r\n   LUID "
                  << AdapterLuidValue(desc.AdapterLuid);
                if (has_umd)
                    s << ", UMD driver " << HIWORD(umd.HighPart) << "." << LOWORD(umd.HighPart) << "." << HIWORD(umd.LowPart) << "." << LOWORD(umd.LowPart);
                s << "\r\n";
            }
            adapter.Reset();
        }
    } else {
        s << "(CreateDXGIFactory1 failed)\r\n";
    }
    s << "\r\n[GPU adapters: WMI Win32_VideoController]\r\n";
    std::string wmi_error;
    const auto controllers = WmiQuery(L"SELECT Name, PNPDeviceID, DriverVersion, DriverDate, AdapterRAM, CurrentHorizontalResolution, "
                                      L"CurrentVerticalResolution, CurrentRefreshRate, VideoProcessor, Status FROM Win32_VideoController",
                                      {L"Name", L"PNPDeviceID", L"DriverVersion", L"DriverDate", L"AdapterRAM", L"CurrentHorizontalResolution",
                                       L"CurrentVerticalResolution", L"CurrentRefreshRate", L"VideoProcessor", L"Status"},
                                      &wmi_error);
    for (const auto& row : controllers) {
        const auto get = [&](const char* key) { const auto it = row.find(key); return it == row.end() ? std::string() : it->second; };
        s << "- " << get("Name") << "\r\n   DriverVersion=" << get("DriverVersion") << " DriverDate=" << get("DriverDate").substr(0, 8)
          << "\r\n   PNPDeviceID=" << get("PNPDeviceID") << "\r\n   AdapterRAM(WMI, 32-bit)=" << get("AdapterRAM")
          << " current=" << get("CurrentHorizontalResolution") << "x" << get("CurrentVerticalResolution") << "@" << get("CurrentRefreshRate")
          << "Hz VideoProcessor=" << get("VideoProcessor") << " Status=" << get("Status") << "\r\n";
    }
    if (controllers.empty()) {
        s << "(no rows" << (wmi_error.empty() ? "" : ": " + wmi_error) << ")\r\n";
        warnings->push_back("WMI Win32_VideoController query returned nothing" + (wmi_error.empty() ? std::string() : ": " + wmi_error));
    }
    s << "\r\n[Display driver registry entries]\r\n";
    const wchar_t* class_key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int i = 0; i < 32; ++i) {
        wchar_t sub[160]{};
        std::swprintf(sub, std::size(sub), L"%ls\\%04d", class_key, i);
        const std::string desc = RegString(HKEY_LOCAL_MACHINE, sub, L"DriverDesc");
        if (desc.empty()) continue;
        std::uint64_t memory_size = 0;
        const bool has_memory = RegNumber(HKEY_LOCAL_MACHINE, sub, L"HardwareInformation.qwMemorySize", &memory_size);
        s << "- " << Hex(static_cast<std::uint32_t>(i), 4) << " " << desc << " DriverVersion=" << RegString(HKEY_LOCAL_MACHINE, sub, L"DriverVersion")
          << " DriverDate=" << RegString(HKEY_LOCAL_MACHINE, sub, L"DriverDate") << " Provider=" << RegString(HKEY_LOCAL_MACHINE, sub, L"ProviderName")
          << " MatchingDeviceId=" << RegString(HKEY_LOCAL_MACHINE, sub, L"MatchingDeviceId")
          << (has_memory ? " qwMemorySize=" + Mib(memory_size) : std::string()) << "\r\n";
    }
    // Displays: current mode and the refresh rates offered at the current resolution.
    s << "\r\n[Displays]\r\n";
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        const bool has_monitor = EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0) != FALSE;
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        const bool has_current = EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &current, 0) != FALSE;
        s << "- " << Utf8(device.DeviceName) << ((device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "")
          << " adapter=" << Utf8(device.DeviceString) << " monitor=" << (has_monitor ? Utf8(monitor.DeviceString) : std::string("?")) << "\r\n";
        if (has_current) {
            s << "   current " << current.dmPelsWidth << "x" << current.dmPelsHeight << " @ " << current.dmDisplayFrequency << " Hz, "
              << current.dmBitsPerPel << " bpp, position (" << current.dmPosition.x << "," << current.dmPosition.y << ")\r\n";
            std::set<DWORD> rates;
            std::set<std::pair<DWORD, DWORD>> sizes;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            for (DWORD m = 0; m < 4096 && EnumDisplaySettingsExW(device.DeviceName, m, &mode, 0); ++m) {
                sizes.insert({mode.dmPelsWidth, mode.dmPelsHeight});
                if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight) rates.insert(mode.dmDisplayFrequency);
            }
            s << "   refresh rates at " << current.dmPelsWidth << "x" << current.dmPelsHeight << ":";
            for (const DWORD rate : rates) s << " " << rate;
            s << " Hz\r\n   resolutions offered: " << sizes.size();
            if (!sizes.empty()) s << " (largest " << sizes.rbegin()->first << "x" << sizes.rbegin()->second << ")";
            s << "\r\n";
        }
    }
    // Host exe and libmpv.
    s << "\r\n[SceneArc]\r\n";
    wchar_t exe_buffer[MAX_PATH * 4]{};
    const DWORD exe_length = GetModuleFileNameW(nullptr, exe_buffer, static_cast<DWORD>(std::size(exe_buffer)));
    const fs::path exe(std::wstring(exe_buffer, exe_length));
    std::uint64_t exe_size = 0;
    FILETIME exe_time{};
    FileAttributes(exe, &exe_size, &exe_time);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const unsigned char*>(dos) + dos->e_lfanew);
    const std::time_t link_time = static_cast<std::time_t>(nt->FileHeader.TimeDateStamp);
    std::tm link_tm{};
    char link_text[32]{};
    if (localtime_s(&link_tm, &link_time) == 0) std::strftime(link_text, sizeof(link_text), "%Y-%m-%d %H:%M:%S", &link_tm);
    s << "Exe: " << Utf8(exe.wstring()) << "\r\n   size " << exe_size << " bytes, modified " << FormatFileTime(exe_time)
      << ", PE link time " << link_text << ", compiled " << __DATE__ << " " << __TIME__ << "\r\n   SHA-256 " << Sha256File(exe) << "\r\n";
    s << "Location: " << InstallKind(req.exe_dir, req.app_root) << "\r\n";
    s << "Data folder: " << Utf8(req.data_dir.wstring()) << "\r\n";
    const fs::path libmpv = req.exe_dir / L"libmpv-2.dll";
    std::uint64_t mpv_size = 0;
    FILETIME mpv_time{};
    const bool has_libmpv = FileAttributes(libmpv, &mpv_size, &mpv_time);
    s << "libmpv: " << (req.mpv_version.empty() ? "(mpv-version unavailable: libmpv " + std::string(req.player_ready ? "ready but version empty" : "not initialised") + ")"
                                                 : req.mpv_version + " (mpv-version property)") << "\r\n";
    if (!req.ffmpeg_version.empty()) s << "FFmpeg (in libmpv): " << req.ffmpeg_version << "\r\n";
    if (has_libmpv) {
        const std::string version = FileVersionText(libmpv);
        s << "libmpv-2.dll: file version " << (version.empty() ? "(none)" : version) << ", size " << mpv_size << " bytes, modified "
          << FormatFileTime(mpv_time) << "\r\n   SHA-256 " << Sha256File(libmpv) << "\r\n";
    } else {
        s << "libmpv-2.dll: missing next to the exe\r\n";
    }
    return s.str();
}

std::string RuntimeText(const Request& req, std::vector<std::string>* warnings) {
    std::ostringstream s;
    const fs::path runtime = req.exe_dir / L"vs-runtime";
    const fs::path plugins = runtime / L"vs-plugins";
    s << "SceneArc diagnostics: VapourSynth / AI runtime\r\nCreated: " << req.created << "\r\nvs-runtime: "
      << Utf8(runtime.wstring()) << "\r\n\r\n";
    s << "[vs-runtime\\RUNTIME_VERSION.txt]\r\n";
    const std::string version_text = ReadFileUtf8(runtime / L"RUNTIME_VERSION.txt");
    s << (version_text.empty() ? std::string("(missing)\r\n") : version_text) << (version_text.empty() || version_text.back() == '\n' ? "" : "\r\n");
    const fs::path site = runtime / L"Lib" / L"site-packages";
    const std::string k7s = FirstLineContaining(site / L"k7sfunc" / L"__init__.py", "__version__");
    s << "\r\n[Python packages]\r\nk7sfunc/__init__.py: " << (k7s.empty() ? "(not found)" : k7s) << "\r\n";
    std::string vsmlrt_line;
    fs::path vsmlrt_path;
    for (const fs::path& candidate : {site / L"k7sfunc" / L"_external" / L"vsmlrt.py", site / L"vsmlrt.py", plugins / L"vsmlrt.py"}) {
        vsmlrt_line = FirstLineContaining(candidate, "__version__");
        if (!vsmlrt_line.empty()) { vsmlrt_path = candidate; break; }
    }
    std::error_code relative_ec;
    s << "vsmlrt.py: " << (vsmlrt_line.empty() ? "(not found)"
                                               : vsmlrt_line + "  [" + Utf8(fs::relative(vsmlrt_path, runtime, relative_ec).wstring()) + "]") << "\r\n";

    // TensorRT install gate (the same checks the player uses) and the model exports.
    std::string missing;
    const bool trt = vp::TensorRtRuntimeInstalled(req.exe_dir, &missing);
    const vp::TensorRtModelExports exports = vp::TensorRtModelExportsPresent(req.exe_dir);
    s << "\r\n[TensorRT gate: TensorRtRuntimeInstalled]\r\n" << (trt ? std::string("installed") : "missing:" + missing) << "\r\nTRT model exports present:";
    for (const auto& name : exports.present) s << " " << name;
    s << "\r\nTRT model exports missing:";
    for (const auto& name : exports.missing) s << " " << name;
    s << "\r\n";

    // Recursive listing; hashes of the inference DLLs wherever they sit (vs-plugins\ or vs-plugins\vsort\).
    s << "\r\n[vs-plugins listing: size bytes, modified, path]\r\n";
    const std::set<std::string> hashed = {"vsort.dll", "vstrt.dll", "onnxruntime.dll", "directml.dll", "trtexec.exe"};
    std::vector<std::pair<std::string, fs::path>> hash_targets;
    std::vector<std::string> engines;
    std::uint64_t file_count = 0, total = 0;
    std::error_code ec;
    if (fs::is_directory(plugins, ec)) {
        std::vector<std::tuple<std::string, std::uint64_t, std::string, fs::path>> files;
        for (auto it = fs::recursive_directory_iterator(plugins, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::uint64_t size = 0;
            FILETIME modified{};
            FileAttributes(it->path(), &size, &modified);
            const std::string relative = Utf8(fs::relative(it->path(), plugins, ec).wstring());
            files.emplace_back(relative, size, FormatFileTime(modified), it->path());
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return Lower(std::get<0>(a)) < Lower(std::get<0>(b)); });
        for (const auto& [relative, size, modified, path] : files) {
            char size_text[32]{};
            std::snprintf(size_text, sizeof(size_text), "%14llu", static_cast<unsigned long long>(size));
            s << size_text << "  " << modified << "  " << relative << "\r\n";
            ++file_count;
            total += size;
            const std::string name = Lower(Utf8(path.filename().wstring()));
            if (hashed.count(name)) hash_targets.emplace_back(relative, path);
            if (name.size() > 7 && name.compare(name.size() - 7, 7, ".engine") == 0)
                engines.push_back(std::string(size_text) + "  " + modified + "  " + relative);
        }
        s << file_count << " files, " << Mib(total) << "\r\n";
    } else {
        s << "(vs-plugins folder missing)\r\n";
        warnings->push_back("vs-runtime\\vs-plugins is missing");
    }

    s << "\r\n[SHA-256]\r\n";
    for (const char* wanted : {"vsort.dll", "vstrt.dll", "onnxruntime.dll", "directml.dll", "trtexec.exe"}) {
        bool found = false;
        for (const auto& [relative, path] : hash_targets) {
            if (Lower(Utf8(path.filename().wstring())) != wanted) continue;
            found = true;
            s << Sha256File(path) << "  " << relative << "\r\n";
        }
        if (!found) s << "(absent)  " << wanted << "\r\n";
    }

    s << "\r\n[vs-mlrt release tags inside the plugin DLLs]\r\n";
    for (const char* wanted : {"vstrt.dll", "vsort.dll"}) {
        bool found = false;
        for (const auto& [relative, path] : hash_targets) {
            if (Lower(Utf8(path.filename().wstring())) != wanted) continue;
            found = true;
            const FileRead bytes = ReadShared(path, 0, false);
            const std::vector<std::string> tags = bytes.ok ? FindReleaseTags(bytes.data) : std::vector<std::string>{};
            s << relative << ":";
            for (const auto& tag : tags) s << " " << tag;
            if (tags.empty()) s << " (no vNN.NN tag found)";
            if (std::string(wanted) == "vstrt.dll")
                s << (std::find(tags.begin(), tags.end(), "v15.16-0-g885e8bb") != tags.end() ? "  => vs-mlrt v15.16 build (expected)"
                                                                                            : "  => NOT the vs-mlrt v15.16 build (v15.16-0-g885e8bb)");
            s << "\r\n";
        }
        if (!found) s << wanted << ": absent\r\n";
    }

    s << "\r\n[TensorRT engines under vs-plugins\\models]\r\n";
    for (const auto& line : engines) s << line << "\r\n";
    if (engines.empty()) s << "(none)\r\n";
    // vsmlrt falls back to %TEMP%\<exe path without the drive>\vs-runtime\vs-plugins\models when models\ is read-only.
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD temp_length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    const std::wstring exe_dir_text = req.exe_dir.wstring();
    if (temp_length && exe_dir_text.size() > 3) {
        const fs::path mirror = fs::path(std::wstring(temp, temp_length)) / exe_dir_text.substr(3) / L"vs-runtime" / L"vs-plugins" / L"models";
        s << "\r\n[TensorRT engines in the %TEMP% mirror: " << Utf8(mirror.wstring()) << "]\r\n";
        std::size_t mirror_count = 0;
        if (fs::is_directory(mirror, ec)) {
            for (auto it = fs::recursive_directory_iterator(mirror, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || Lower(Utf8(it->path().extension().wstring())) != ".engine") continue;
                std::uint64_t size = 0;
                FILETIME modified{};
                FileAttributes(it->path(), &size, &modified);
                s << size << "  " << FormatFileTime(modified) << "  " << Utf8(fs::relative(it->path(), mirror, ec).wstring()) << "\r\n";
                ++mirror_count;
            }
        }
        if (!mirror_count) s << "(none)\r\n";
    }
    return s.str();
}

// config-video.json: only video, audio and mpvLan.port (never libraries, LAN users/tokens, API keys or paths).
std::string ConfigVideoJson(const std::string& raw_config) {
    bool valid = false;
    const vp::Json config = vp::Json::Parse(raw_config, &valid);
    if (!valid || !config.is_object()) return "{\"error\":\"config.json could not be parsed\"}\n";
    const auto section = [&](const char* key) {
        return config[key].is_object() ? vp::mpv_config_safety::SerializeJson(config[key]) : std::string("null");
    };
    const vp::Json& port = config["mpvLan"]["port"];
    return PrettyJson("{\"video\":" + section("video") + ",\"audio\":" + section("audio") + ",\"mpvLan\":{\"port\":" +
                      (port.type() == vp::Json::Type::Number ? vp::mpv_config_safety::SerializeJson(port) : std::string("null")) + "}}");
}

Result Build(const Request& req) {
    const auto started = std::chrono::steady_clock::now();
    Result result;
    result.zip_path = req.zip_path;
    std::error_code ec;
    fs::create_directories(req.zip_path.parent_path(), ec);
    // Leftovers of an export interrupted by closing the player.
    for (const auto& entry : fs::directory_iterator(req.zip_path.parent_path(), ec))
        if (entry.path().extension() == L".partial") fs::remove(entry.path(), ec);
    const fs::path partial = req.zip_path.wstring() + L".partial";
    ZipWriter zip(partial);
    if (!zip.ok()) {
        result.error = "cannot create " + Utf8(partial.wstring());
        return result;
    }
    std::vector<std::string> notes; // README lines about truncation and skipped files
    const auto add = [&](const std::string& name, const std::string& data, const FILETIME* modified) {
        if (zip.Add(name, data, modified)) result.items.push_back(name);
        else result.warnings.push_back("zip write failed at " + name);
    };

    // logs/: the three logs (tail 5 MB each).
    const fs::path logs = req.data_dir / L"logs";
    for (const wchar_t* name : {L"mpv-host.log", L"mpv.log", L"vs-trt.log"}) {
        const fs::path path = logs / name;
        if (!fs::exists(path, ec)) { notes.push_back("logs/" + Utf8(name) + "：不存在（未產生）"); continue; }
        const FileRead file = ReadShared(path, kLogTailBytes, true);
        if (!file.ok) { result.warnings.push_back("cannot read " + Utf8(path.wstring()) + " (error " + std::to_string(file.error) + ")"); continue; }
        if (file.truncated) notes.push_back("logs/" + Utf8(name) + "：原始 " + Mib(file.size) + "，只收錄最後 5 MB（truncated to the last 5 MB）");
        add("logs/" + Utf8(name), file.data, &file.modified);
    }
    // logs/crash/*: newest first, 50 MB in total.
    const fs::path crash = logs / L"crash";
    if (fs::is_directory(crash, ec)) {
        std::vector<std::tuple<std::uint64_t, std::uint64_t, fs::path>> files; // (mtime, size, path)
        for (const auto& entry : fs::directory_iterator(crash, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::uint64_t size = 0;
            FILETIME modified{};
            FileAttributes(entry.path(), &size, &modified);
            files.emplace_back((static_cast<std::uint64_t>(modified.dwHighDateTime) << 32) | modified.dwLowDateTime, size, entry.path());
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        std::uint64_t used = 0;
        for (const auto& [mtime, size, path] : files) {
            const std::string name = "logs/crash/" + Utf8(path.filename().wstring());
            if (used + size > kCrashBudgetBytes) {
                notes.push_back(name + "：略過（" + Mib(size) + "，超過崩潰檔 50 MB 總上限 / skipped, over the 50 MB crash-dump budget）");
                continue;
            }
            const FileRead file = ReadShared(path, 0, false);
            if (!file.ok) { result.warnings.push_back("cannot read " + Utf8(path.wstring())); continue; }
            used += file.data.size();
            add(name, file.data, &file.modified);
        }
    }
    // %TEMP%\trtexec_*.log: newest 5.
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD temp_length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (temp_length) {
        const fs::path temp_dir(std::wstring(temp, temp_length));
        std::vector<std::pair<std::uint64_t, fs::path>> trtexec;
        for (const auto& entry : fs::directory_iterator(temp_dir, fs::directory_options::skip_permission_denied, ec)) {
            const std::string name = Lower(Utf8(entry.path().filename().wstring()));
            if (name.rfind("trtexec_", 0) != 0 || name.size() < 12 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
            FILETIME modified{};
            if (FileAttributes(entry.path(), nullptr, &modified))
                trtexec.emplace_back((static_cast<std::uint64_t>(modified.dwHighDateTime) << 32) | modified.dwLowDateTime, entry.path());
        }
        std::sort(trtexec.begin(), trtexec.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (trtexec.size() > kTrtexecLogs) notes.push_back("trtexec/：%TEMP% 有 " + std::to_string(trtexec.size()) + " 個 trtexec_*.log，只收錄最新 5 個");
        for (std::size_t i = 0; i < trtexec.size() && i < kTrtexecLogs; ++i) {
            const FileRead file = ReadShared(trtexec[i].second, kLogTailBytes, true);
            if (!file.ok) continue;
            const std::string name = "trtexec/" + Utf8(trtexec[i].second.filename().wstring());
            if (file.truncated) notes.push_back(name + "：只收錄最後 5 MB");
            add(name, file.data, &file.modified);
        }
    }
    // cache/vs/*.vpy: the generated scripts (newest 200).
    const fs::path vs_cache = req.data_dir / L"cache" / L"vs";
    if (fs::is_directory(vs_cache, ec)) {
        std::vector<std::pair<std::uint64_t, fs::path>> scripts;
        for (const auto& entry : fs::directory_iterator(vs_cache, ec)) {
            if (Lower(Utf8(entry.path().extension().wstring())) != ".vpy") continue;
            FILETIME modified{};
            if (FileAttributes(entry.path(), nullptr, &modified))
                scripts.emplace_back((static_cast<std::uint64_t>(modified.dwHighDateTime) << 32) | modified.dwLowDateTime, entry.path());
        }
        std::sort(scripts.begin(), scripts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (scripts.size() > kVpyMax) notes.push_back("cache/vs/：共 " + std::to_string(scripts.size()) + " 個 .vpy，只收錄最新 200 個");
        for (std::size_t i = 0; i < scripts.size() && i < kVpyMax; ++i) {
            const FileRead file = ReadShared(scripts[i].second, 0, false);
            if (file.ok) add("cache/vs/" + Utf8(scripts[i].second.filename().wstring()), file.data, &file.modified);
        }
    }

    add("config-video.json", ConfigVideoJson(req.raw_config), nullptr);
    // COM for WMI (Win32_VideoController.DriverVersion) on this worker only.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    add("system.txt", SystemText(req, &result.warnings), nullptr);
    if (SUCCEEDED(com)) CoUninitialize();
    add("runtime.txt", RuntimeText(req, &result.warnings), nullptr);
    add("state.json", PrettyJson(req.state_json), nullptr);
    if (!req.refresh_summary.empty()) add("refresh-summary.txt", "\xEF\xBB\xBF" + req.refresh_summary, nullptr);

    std::ostringstream readme;
    readme << "\xEF\xBB\xBF" // UTF-8 BOM for Notepad
           << "SceneArc 診斷資料\r\n"
           << "請把此檔案提供給開發者。\r\n\r\n"
           << "建立時間：" << req.created << "\r\n"
           << "程式版本：VideoPlayer.exe compiled " << __DATE__ << " " << __TIME__ << "（詳見 system.txt）\r\n"
           << "程式位置：" << Utf8(req.exe_dir.wstring()) << "（" << InstallKind(req.exe_dir, req.app_root) << "）\r\n\r\n"
           << "內容：\r\n"
           << "  logs/            mpv-host.log、mpv.log、vs-trt.log（每個最多最後 5 MB）與 crash/（崩潰檔，總計最多 50 MB）\r\n"
           << "  trtexec/         %TEMP%\\trtexec_*.log（最新 5 個，TensorRT 建引擎紀錄）\r\n"
           << "  cache/vs/        播放器產生的 VapourSynth 腳本（*.vpy）\r\n"
           << "  config-video.json  目前設定中的 video、audio 區段與 mpvLan.port\r\n"
           << "  system.txt       Windows、CPU、記憶體、GPU（驅動版本、VRAM）、螢幕更新率、播放器與 libmpv 版本\r\n"
           << "  runtime.txt      vs-runtime 版本、vs-plugins 檔案清單、DLL 雜湊、vstrt.dll 版本標記、TensorRT 檢查結果與引擎清單\r\n"
           << "  state.json       匯出當下的 frameProcessing／opened 狀態、AI 推理後端與原因、VapourSynth 初始化狀態\r\n"
           << (req.refresh_summary.empty() ? "" : "  refresh-summary.txt  最近幾次「刷新全部中繼資料」的摘要（由介面隨匯出要求送來，最多 64 KB）\r\n")
           << "\r\n"
           << "未包含（隱私）：\r\n"
           << "  config.json 的其他部分都沒有收錄：媒體庫名稱與資料夾路徑、LAN 使用者與權杖、TMDB／Bangumi 金鑰、觀看進度、\r\n"
           << "  縮圖與海報快取、WebView2 資料。注意：mpv.log、mpv-host.log 與 state.json 會含有正在播放（或最近播放）的影片檔名與路徑。\r\n";
    if (!notes.empty()) {
        readme << "\r\n截斷與略過：\r\n";
        for (const auto& note : notes) readme << "  " << note << "\r\n";
    }
    if (!result.warnings.empty()) {
        readme << "\r\n警告：\r\n";
        for (const auto& warning : result.warnings) readme << "  " << warning << "\r\n";
    }
    readme << "\r\n收錄檔案（" << result.items.size() + 1 << "）：\r\n";
    for (const auto& item : result.items) readme << "  " << item << "\r\n";
    readme << "  README.txt\r\n";
    add("README.txt", readme.str(), nullptr);

    if (!zip.Finish()) {
        result.error = "zip write failed (disk full or path not writable)";
        fs::remove(partial, ec);
        return result;
    }
    result.bytes = zip.bytes();
    if (!MoveFileExW(partial.c_str(), req.zip_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = "cannot rename the zip (error " + std::to_string(GetLastError()) + ")";
        fs::remove(partial, ec);
        return result;
    }
    result.ok = true;
    result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return result;
}

std::string StringArray(const std::vector<std::string>& items) {
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(items[i]);
    return json + "]";
}

std::atomic<bool> g_running{false};

} // namespace diag

// Runs a closure on the UI thread (worker -> UI hand-off for results that touch g.player or the WebView).
void RunOnUiThread(std::function<void()> work) {
    auto* payload = new std::function<void()>(std::move(work));
    if (!g.window || !PostMessageW(g.window, WM_APP_RUN, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

// exportDiagnostics: snapshot here (UI thread: backend state and config), build on a worker, finish on the UI thread.
// Events: {type:"diagnostics", state:"start"|"done"|"error", path, ...}; reply diagnosticsExport {ok,path,bytes,items,warnings}.
void StartDiagnosticsExport(const vp::Json& message) {
    if (diag::g_running.exchange(true)) {
        PostToWeb(Reply("diagnosticsExport", message, "\"ok\":false,\"reason\":\"busy\",\"path\":\"\",\"bytes\":0,\"items\":[],\"warnings\":[]"));
        return;
    }
    diag::Request req;
    req.exe_dir = ExeDir();
    req.data_dir = DataDir();
    req.app_root = g.root;
    req.created = diag::NowText(false);
    const fs::path folder = req.data_dir / L"diagnostics";
    const std::wstring stamp = Wide(diag::NowText(true));
    req.zip_path = folder / (L"vp-diag-" + stamp + L".zip");
    std::error_code ec;
    for (int i = 2; fs::exists(req.zip_path, ec) && i < 100; ++i) req.zip_path = folder / (L"vp-diag-" + stamp + L"-" + std::to_wstring(i) + L".zip");
    req.raw_config = LoadRawConfigText();
    // 1B-b H6: the request's optional summary (a string as it is, or JSON pretty-printed), else ui.metaRefresh (what
    // the page sends today: the last 刷新全部中繼資料 summaries). Capped at 64 KB on a UTF-8 character boundary.
    {
        const vp::Json& summary = message["summary"];
        const vp::Json& refresh = summary.is_null() ? message["ui"]["metaRefresh"] : summary;
        const bool nothing = refresh.is_null() || (refresh.is_array() && refresh.items().empty()) || (refresh.is_object() && refresh.members().empty());
        std::string text = refresh.is_string() ? refresh.str() : nothing ? std::string() : diag::PrettyJson(JsonText(refresh));
        constexpr std::size_t kSummaryCap = 64 * 1024;
        if (text.size() > kSummaryCap) {
            const std::size_t total = text.size();
            std::size_t cut = kSummaryCap;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text.resize(cut);
            text += "\r\n... (truncated: " + std::to_string(total) + " bytes in the request, first " + std::to_string(cut) + " kept)\r\n";
        }
        req.refresh_summary = std::move(text);
    }
    const auto raw_or_null = [](const std::string& json) { return json.empty() ? std::string("null") : json; };
    if (g.player) {
        req.player_ready = g.player->Ready();
        req.mpv_version = g.player->MpvVersion();
        req.ffmpeg_version = g.player->FfmpegVersion();
        char busy_ms[32]{};
        std::snprintf(busy_ms, sizeof(busy_ms), "%.0f", g.player->VsBusyMs());
        req.state_json = "{\"exportedAt\":" + vp::Json::Quote(req.created) + ",\"playerReady\":" + (req.player_ready ? "true" : "false") +
            ",\"playerError\":" + vp::Json::Quote(g.player_error) +
            // The backend is resolved when a file's enhancement is applied; before the first file the reason is empty.
            ",\"aiBackendResolved\":" + (g.player->AiBackendReason().empty() ? "false" : "true") +
            ",\"aiBackend\":" + vp::Json::Quote(g.player->AiBackend()) + ",\"aiBackendReason\":" + vp::Json::Quote(g.player->AiBackendReason()) +
            ",\"trtInstalledAtResolve\":" + (g.player->TrtInstalledAtResolve() ? "true" : "false") +
            ",\"gpuVendor\":" + vp::Json::Quote(g.player->GpuVendor()) +
            ",\"vsBusy\":" + (g.player->VsBusy() ? "true" : "false") + ",\"vsBusyTrigger\":" +
            vp::Json::Quote(g.player->VsBusy() ? g.player->VsBusyTrigger() : std::string()) +
            ",\"vsBusyMs\":" + busy_ms + ",\"vsStages\":" + vp::Json::Quote(g.player->ActiveVsStages()) +
            ",\"vsScripts\":{\"upscale\":" + vp::Json::Quote(g.player->VsUpscaleScript()) + ",\"interpolation\":" +
            vp::Json::Quote(g.player->VsInterpolationScript()) + ",\"chain\":" + vp::Json::Quote(g.player->VsChainScript()) + "}" +
            ",\"mediaActive\":" + (g.player->MediaActive() ? "true" : "false") + ",\"currentPath\":" + vp::Json::Quote(g.player->CurrentPath()) +
            ",\"frameProcessing\":" + raw_or_null(g.player->LastFrameProcessingJson()) + ",\"opened\":" + raw_or_null(g.player->LastOpenedJson()) + "}";
    } else {
        req.state_json = "{\"exportedAt\":" + vp::Json::Quote(req.created) + ",\"playerReady\":false,\"playerError\":" +
                         vp::Json::Quote(g.player_error) + ",\"note\":\"libmpv was not loaded\",\"frameProcessing\":null,\"opened\":null}";
    }
    const std::string path_utf8 = Utf8(req.zip_path.wstring());
    if (g.player) g.player->HostLog("diagnostics export started path=" + path_utf8);
    PostToWeb("{\"type\":\"diagnostics\",\"state\":\"start\",\"path\":" + vp::Json::Quote(path_utf8) + "}");
    std::string request_text = message.is_object() ? vp::mpv_config_safety::SerializeJson(message) : std::string("{}");
    std::thread([req = std::move(req), request_text = std::move(request_text)]() {
        diag::Result result;
        try {
            result = diag::Build(req);
        } catch (const std::exception& error) {
            result.error = std::string("exception: ") + error.what();
        } catch (...) {
            result.error = "unknown exception";
        }
        RunOnUiThread([result = std::move(result), request_text]() {
            const vp::Json request = vp::Json::Parse(request_text);
            const std::string path = Utf8(result.zip_path.wstring());
            char ms[32]{};
            std::snprintf(ms, sizeof(ms), "%.0f", result.ms);
            if (g.player) {
                if (result.ok)
                    g.player->HostLog("diagnostics exported path=" + path + " bytes=" + std::to_string(result.bytes) + " items=" +
                                      std::to_string(result.items.size()) + " warnings=" + std::to_string(result.warnings.size()) + " ms=" + ms);
                else g.player->HostLog("diagnostics export failed path=" + path + " error=" + result.error);
            }
            const std::string fields = std::string("\"ok\":") + (result.ok ? "true" : "false") + ",\"path\":" + vp::Json::Quote(path) +
                ",\"bytes\":" + std::to_string(result.bytes) + ",\"items\":" + diag::StringArray(result.items) +
                ",\"warnings\":" + diag::StringArray(result.warnings) + ",\"reason\":" + vp::Json::Quote(result.error) + ",\"ms\":" + ms;
            PostToWeb(Reply("diagnosticsExport", request, fields));
            PostToWeb("{\"type\":\"diagnostics\",\"state\":" + std::string(result.ok ? "\"done\"" : "\"error\"") + "," + fields +
                      ",\"itemCount\":" + std::to_string(result.items.size()) + ",\"message\":" + vp::Json::Quote(result.error) + "}");
            diag::g_running = false;
        });
    }).detach();
}
#endif

// ---------------------------------------------------------------- web messages
void HandleWebMessage(const std::string& text) {
    bool ok = false;
    const vp::Json message = vp::Json::Parse(text, &ok);
    if (!ok) return;
    const std::string cmd = message["cmd"].str();

    if (cmd == "open") {
        EnsurePlayer(); // mpv: normally already done after NavigationCompleted (WM_APP_WARM_PLAYER); no-op then
#if defined(VP_ENGINE_MPV)
        if (!g.player || !g.player->Ready()) {
            const std::string error = g.player_error.empty() ? "libmpv is unavailable." : g.player_error;
            const std::string code = g.player_error_code.empty() ? "MPV_INIT" : g.player_error_code;
            const int open_result = code == "LIBMPV_MISSING" ? -100 : -1;
            PostToWeb("{\"type\":\"player\",\"event\":\"error\",\"data\":{\"fatal\":true,\"code\":" +
                       vp::Json::Quote(code) + ",\"message\":" + vp::Json::Quote(error) + "}}");
            PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(open_result) + ",\"code\":" + vp::Json::Quote(code) + ",\"engine\":\"mpv\""));
            return;
        }
        // A rejected open (sync or async) is reported once by the backend (MpvBackend::FailLoad), with the state latched.
        const int result = g.player->Open(message["path"].str(), message["startMs"].i64(), message["paused"].boolean(), message["noAudio"].boolean());
        PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(result) + ",\"engine\":\"mpv\""));
#else
        const vp::Json config = vp::Json::Parse(LoadConfigText());
        std::uint64_t gpu_adapter_luid = 0;
        if (!ParseGpuAdapterLuid(config["gpuAdapterLuid"].str(), &gpu_adapter_luid)) {
            PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(PC_E_INVALID_ARG) + ",\"gpuResult\":" +
                            std::to_string(PC_E_INVALID_ARG)));
            return;
        }
        int32_t gpu_result = pc_set_gpu_adapter_luid(g.player, gpu_adapter_luid);
        const bool allow_software_decode_fallback = config["allowSoftwareDecodeFallback"].boolean();
        int32_t software_decode_result = pc_set_software_decode_fallback(g.player, allow_software_decode_fallback ? 1 : 0);
        const bool interpolation = config["frameInterpolation"].boolean();
        const int32_t interpolation_backend = config["frameInterpolationBackend"].str("amf") == "rife"
            ? PC_FRAME_INTERPOLATION_RIFE : PC_FRAME_INTERPOLATION_AMF;
        ApplyRifeModelPath(config);
        std::string restore_model;
        const bool rife_restore = RifeRestoreRequested(config, interpolation, interpolation_backend, &restore_model);
        // RIFE + x2 upscale: upscale only the source frames and warp them with
        // RIFE's native-resolution flow. The core uses it only while an upscaler
        // is active and falls back to the regular RIFE path when the selected
        // model has no "<model>_flow.onnx" export (currently v4.6 FP16 only).
        const bool rife_output_warp = interpolation && interpolation_backend == PC_FRAME_INTERPOLATION_RIFE &&
                                      config["rifeOutputWarp"].boolean();
        int32_t output_warp_result = PC_OK;
        // Duplicate-aware FRC (experimental): skip repeated drawings of animation
        // on twos/threes and interpolate between distinct ones. Applies to AMF
        // and RIFE (the core ignores it together with x1 repair).
        const bool frc_dedup = interpolation && config["frcDedup"].boolean();
        int32_t dedup_result = PC_OK;
        auto configure_interpolation = [&]() {
            const int32_t interpolation_result = pc_set_frame_interpolation_ex(g.player, interpolation ? 1 : 0, interpolation_backend, nullptr);
            const int32_t restore_result = interpolation_result == PC_OK
                ? pc_set_rife_restore(g.player, rife_restore ? 1 : 0, rife_restore ? restore_model.c_str() : nullptr)
                : interpolation_result;
            output_warp_result = pc_set_rife_output_warp(g.player, rife_output_warp ? 1 : 0);
            dedup_result = pc_set_rife_dedup(g.player, frc_dedup ? 1 : 0);
            return std::pair{interpolation_result, restore_result};
        };
        auto [interpolation_result, restore_result] = configure_interpolation();
        const vp::Json vrr = config["vrr"];
        const bool vrr_enabled = vrr["enabled"].boolean();
        const double vrr_min_hz = vrr["minHz"].num();
        const double vrr_max_hz = vrr["maxHz"].num();
        const bool vrr_valid = !vrr_enabled ||
            (std::isfinite(vrr_min_hz) && std::isfinite(vrr_max_hz) &&
             vrr_min_hz >= 20.0 && vrr_max_hz >= vrr_min_hz && vrr_max_hz <= 1000.0);
        const double applied_vrr_min_hz = vrr_enabled && vrr_valid ? vrr_min_hz : 0.0;
        const double applied_vrr_max_hz = vrr_enabled && vrr_valid ? vrr_max_hz : 0.0;
        int32_t vrr_result = pc_set_vrr_range(g.player, applied_vrr_min_hz, applied_vrr_max_hz);
        // The core requires this option before pc_open. When the user starts a
        // different title directly, finish the preceding session first so the
        // new global setting cannot be silently ignored.
        if (interpolation_result == PC_E_STATE || restore_result == PC_E_STATE || output_warp_result == PC_E_STATE ||
            dedup_result == PC_E_STATE ||
            vrr_result == PC_E_STATE || gpu_result == PC_E_STATE || software_decode_result == PC_E_STATE) {
            pc_stop(g.player);
            std::tie(interpolation_result, restore_result) = configure_interpolation();
            vrr_result = pc_set_vrr_range(g.player, applied_vrr_min_hz, applied_vrr_max_hz);
            gpu_result = pc_set_gpu_adapter_luid(g.player, gpu_adapter_luid);
            software_decode_result = pc_set_software_decode_fallback(g.player, allow_software_decode_fallback ? 1 : 0);
        }
        if (!vrr_valid) vrr_result = PC_E_INVALID_ARG;
        // x1 restore and x2 upscaling may now run together.
        const int32_t upscale_result = ApplyUpscaleConfig(config["upscale"]);
        uint32_t flags = 0;
        if (message["paused"].boolean()) flags |= PC_OPEN_PAUSED;
        if (message["noAudio"].boolean()) flags |= PC_OPEN_NO_AUDIO;
        const int32_t result = pc_open(g.player, message["path"].str().c_str(), message["startMs"].i64(), flags);
        PostToWeb(Reply("openResult", message, "\"result\":" + std::to_string(result) + ",\"frameInterpolationResult\":" + std::to_string(interpolation_result) +
                       ",\"rifeRestoreResult\":" + std::to_string(restore_result) +
                       ",\"rifeOutputWarpResult\":" + std::to_string(output_warp_result) +
                       ",\"frcDedupResult\":" + std::to_string(dedup_result) + ",\"vrrResult\":" + std::to_string(vrr_result) +
                       ",\"upscaleResult\":" + std::to_string(upscale_result) + ",\"gpuResult\":" + std::to_string(gpu_result) +
                       ",\"softwareDecodeFallbackResult\":" + std::to_string(software_decode_result)));
#endif
    } else if (cmd == "setUpscale") {
#if defined(VP_ENGINE_MPV)
        EnsurePlayer();
        const bool ready = g.player && g.player->Ready();
        PostToWeb(Reply("enhancementResult", message, std::string("\"result\":") + (ready ? "0" : "-1") +
                        ",\"reason\":\"catalog_pending\""));
#else
        // Live switch from the player / settings; the UI also saves it to config.json.
        EnsurePlayer();
        const int32_t result = ApplyUpscaleConfig(message);
        PostToWeb(Reply("upscaleResult", message, "\"result\":" + std::to_string(result)));
#endif
#if defined(VP_ENGINE_MPV)
    } else if (cmd == "setEnhancement") {
        EnsurePlayer();
        const bool ready = g.player && g.player->Ready();
        const bool applied = ready && message["video"].is_object() && g.player->ApplyEnhancement(message["video"]);
        PostToWeb(Reply("enhancementResult", message, std::string("\"ok\":") + (applied ? "true" : "false") +
                        ",\"reason\":" + vp::Json::Quote(ready ? (applied ? "" : "apply_failed") : "player_unavailable")));
    } else if (cmd == "configureAudio") {
        EnsurePlayer();
        const bool configured = g.player && g.player->Ready() && message["audio"].is_object() && g.player->ConfigureAudio(message["audio"]);
        PostToWeb(Reply("audioConfig", message, std::string("\"ok\":") + (configured ? "true" : "false")));
    } else if (cmd == "play") { if (g.player && g.player->Ready()) g.player->Play(); }
    else if (cmd == "pause") { if (g.player && g.player->Ready()) g.player->Pause(); }
    else if (cmd == "toggle") { if (g.player && g.player->Ready()) g.player->Toggle(); }
    // User seeks follow playback.seekMode; "exact": true (chapter starts, 重新播放) forces an exact seek.
    else if (cmd == "seek") { if (g.player && g.player->Ready()) g.player->SeekUser(message["ms"].i64(), false, message["exact"].boolean(false)); }
    else if (cmd == "seekRelative") { if (g.player && g.player->Ready()) g.player->SeekUser(message["ms"].i64(), true, message["exact"].boolean(false)); }
    else if (cmd == "setVolume") { if (g.player && g.player->Ready()) g.player->SetVolume(static_cast<int>(message["value"].i64(100))); }
    else if (cmd == "setMute") { if (g.player && g.player->Ready()) g.player->SetMute(message["value"].boolean(false)); }
    else if (cmd == "selectSubtitle") { if (g.player && g.player->Ready()) g.player->SelectSubtitle(message["index"].i64(-1)); }
    else if (cmd == "loadSubtitle") { if (g.player && g.player->Ready()) g.player->LoadSubtitle(message["path"].str()); }
    else if (cmd == "selectAudio") { if (g.player && g.player->Ready()) g.player->SelectAudio(message["id"].i64(-1)); }
    else if (cmd == "stop") {
        // The surface stays up (black once mpv unloads) until the UI's releaseSurface.
        if (g.player && g.player->Ready()) g.player->Stop();
#else
    } else if (cmd == "play") { if (g.player) pc_play(g.player); }
    else if (cmd == "pause") { if (g.player) pc_pause(g.player); }
    else if (cmd == "toggle") { if (g.player) pc_toggle_pause(g.player); }
    else if (cmd == "seek") { if (g.player) pc_seek(g.player, message["ms"].i64()); }
    else if (cmd == "seekRelative") { if (g.player) pc_seek_relative(g.player, message["ms"].i64()); }
    else if (cmd == "selectSubtitle") { if (g.player) pc_select_subtitle(g.player, static_cast<int32_t>(message["index"].i64(-1))); }
    else if (cmd == "loadSubtitle") { if (g.player) pc_load_subtitle_file(g.player, message["path"].str().c_str()); }
    else if (cmd == "stop") {
        if (g.player) pc_stop(g.player);
        ShowVideoSurface(false);
#endif
#if defined(VP_ENGINE_MPV) && defined(VP_TEST_HARNESS)
    } else if (cmd == "debugInjectDeviceLost") {
        if (g.player && g.player->Ready()) g.player->DebugInjectLogLine("vo/gpu-next: DXGI_ERROR_DEVICE_HUNG (injected debug validation)");
#endif
    } else if (cmd == "releaseSurface") {
        ReleaseVideoSurface();
    } else if (cmd == "fullscreen") {
        SetFullscreen(message["on"].type() == vp::Json::Type::Bool ? message["on"].boolean() : !g.fullscreen);
    } else if (cmd == "getGpuAdapters") {
#if defined(VP_ENGINE_MPV)
        // 1A: the same snapshot type as the preflight, plus subSysId / revision (identity) and mpvAddressable, and the
        // last preflight status (null before the first attempt) for a UI that missed the gpuAdapterStatus push.
        const std::string status = g.player ? g.player->AdapterStatusJson() : std::string();
        PostToWeb(Reply("gpuAdapters", message, "\"adapters\":" + vp::gpu::AdapterListJson(vp::gpu::EnumerateAdapters()) +
                        ",\"status\":" + (status.empty() ? std::string("null") : status)));
#else
        PostToWeb(Reply("gpuAdapters", message, "\"adapters\":" + EnumerateGpuAdaptersJson()));
#endif
    } else if (cmd == "getEnhancementCatalog") {
#if defined(VP_ENGINE_MPV)
        const std::string catalog = ReadFileUtf8(ExeDir() / L"mpv" / L"catalog.json");
        std::string trt_missing;
        const bool trt_installed = vp::TensorRtRuntimeInstalled(ExeDir(), &trt_missing);
        const vp::TensorRtModelExports trt_exports = vp::TensorRtModelExportsPresent(ExeDir());
        const auto string_array = [](const std::vector<std::string>& items) {
            std::string json = "[";
            for (std::size_t i = 0; i < items.size(); ++i) json += (i ? "," : "") + vp::Json::Quote(items[i]);
            return json + "]";
        };
        PostToWeb(Reply("enhancementCatalog", message, "\"text\":" + vp::Json::Quote(catalog) +
                        ",\"aiModels\":" + EnhancementAiModelsJson() + ",\"modelCatalog\":" + vp::Json::Quote(EnhancementModelCatalogText()) +
                        ",\"rifeModels\":" + EnhancementRifeModelsJson() + ",\"drbaModels\":" + EnhancementDrbaModelsJson() +
                        ",\"vsScripts\":" + EnhancementVsScriptsJson() +
                        ",\"trtInstalled\":" + std::string(trt_installed ? "true" : "false") +
                        // First missing runtime item (gate order) and the TensorRT model exports present/missing under models/.
                        ",\"trtMissing\":" + vp::Json::Quote(trt_missing) +
                        ",\"trtModels\":{\"present\":" + string_array(trt_exports.present) + ",\"missing\":" + string_array(trt_exports.missing) + "}" +
                        ",\"shaderLibrary\":" + EnhancementShaderLibraryJson()));
#else
        PostToWeb(Reply("enhancementCatalog", message, "\"text\":\"\""));
#endif
#if defined(VP_ENGINE_MPV)
    } else if (cmd == "openRuntimeFolder") {
        // Settings 「開啟 TensorRT DLL 資料夾」: Explorer at <exe_dir>\vs-runtime\vs-plugins (created if missing), where the
        // vsNV files go; what:"diagnostics" opens data\diagnostics (the 「匯出診斷資料」 toast). Only known folders resolved
        // from the exe directory; the reply carries the resolved path.
        const std::string what = message["what"].str("vs-plugins");
        fs::path target;
        if (what == "vs-plugins") target = ExeDir() / L"vs-runtime" / L"vs-plugins";
        else if (what == "diagnostics") target = DataDir() / L"diagnostics";
        bool ok = false;
        std::string reason;
        if (target.empty()) reason = "unknown_folder";
        else {
            std::error_code ec;
            fs::create_directories(target, ec);
            if (!fs::is_directory(target, ec)) reason = "create_failed";
            else {
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                ok = result > 32;
                if (!ok) reason = "shell_execute_" + std::to_string(static_cast<long long>(result));
            }
        }
        const std::string target_utf8 = target.empty() ? std::string() : Utf8(target.wstring());
        if (g.player) g.player->HostLog("open runtime folder what=" + what + " path=" + target_utf8 + " result=" + (ok ? "ok" : reason));
        PostToWeb(Reply("runtimeFolder", message, std::string("\"ok\":") + (ok ? "true" : "false") + ",\"what\":" + vp::Json::Quote(what) +
                        ",\"path\":" + vp::Json::Quote(target_utf8) + ",\"reason\":" + vp::Json::Quote(reason)));
    } else if (cmd == "exportDiagnostics") {
        // 「匯出診斷資料」: zip under data\diagnostics, built on a worker (StartDiagnosticsExport). Local WebView only:
        // HandleLanRpc's allow-list refuses it for LAN clients.
        StartDiagnosticsExport(message);
#endif
    } else if (cmd == "getConfig") {
        PostToWeb(Reply("config", message, "\"text\":" + vp::Json::Quote(LoadConfigText()) + "," + AudioStatusJson() +
#if defined(VP_ENGINE_MPV)
                        ",\"engine\":\"mpv\""));
#else
                        ",\"engine\":\"playbackcore\""));
#endif
    } else if (cmd == "setConfig") {
        const std::string config_text = message["text"].str();
        bool valid = false;
        const vp::Json candidate = vp::Json::Parse(config_text, &valid);
#if defined(VP_ENGINE_MPV)
        const bool saved = valid && candidate.is_object() && WriteMpvConfigText(config_text);
        // playback.seekMode applies to the next seek; nothing else in the saved text is applied here.
        if (saved && g.player && g.player->Ready()) g.player->SetSeekPreference(candidate["playback"]["seekMode"].str("exact"));
        // 1A: while the adapter preflight blocks (libmpv never loaded), a saved re-selection / 改用自動 is tried at once.
        if (saved && g.player && g.player->AdapterPreflightBlocked() && !g.player->InitializationAttempted()) {
            g.player->HostLog("gpu adapter selection saved while blocked; retrying initialization in-process");
            PostMessageW(g.window, WM_APP_WARM_PLAYER, 0, 0);
        }
#else
        const bool saved = valid && WriteFileUtf8(ConfigPath(), config_text);
#endif
        if (saved && message["restartAudio"].boolean()) {
#if !defined(VP_ENGINE_MPV)
            StartAudioHost();
#endif
        }
        PostToWeb(Reply("configSaved", message, std::string("\"ok\":") + (saved ? "true" : "false") + "," + AudioStatusJson()));
    } else if (cmd == "loadData") {
        // state: "ok" | "missing" | "error" | "invalid-key"; for "progress" also "empty" | "invalid" (1B-b H4), and
        // "readOnly":true when progress saves are refused from now on (see ProgressWritable).
        const std::string key = message["key"].str();
        const char* state = "invalid-key";
        std::string data;
        bool read_only = false;
        if (key == "progress") {
            DataFileCheck check = g_lan_mode == "server" ? LoadDefaultLanProgress() : CheckJsonObjectFile(DataDir() / L"progress.json");
            state = check.state;
            data = std::move(check.text);
            SetProgressGuard(state, "loadData");
            read_only = g_progress_guard == DataGuard::ReadOnly;
        } else if (SafeKey(key)) {
            data = ReadFileUtf8Status(DataDir() / (Wide(key) + L".json"), &state);
        }
        PostToWeb(Reply("data", message, "\"key\":" + vp::Json::Quote(key) + ",\"state\":" + vp::Json::Quote(state) +
            (read_only ? ",\"readOnly\":true" : "") + ",\"text\":" + vp::Json::Quote(data)));
    } else if (cmd == "saveData") {
        // dataSaved {ok, reason?}: reason "invalid-key" | "read-only" (progress, 1B-b H4) | "invalid-json" (1B-b H2:
        // anilist / prefs / progress must be one JSON object; the file is not touched) | "write-failed".
        const std::string key = message["key"].str();
        const std::string text = message["text"].str();
        const char* reason = "";
        bool saved = false;
        std::string why;
        if (!SafeKey(key)) {
            reason = "invalid-key";
        } else if (key == "progress" && !ProgressWritable()) {
            reason = "read-only";
        } else if ((key == "anilist" || key == "prefs" || key == "progress") && !IsJsonObjectText(text, &why)) {
            reason = "invalid-json";
            DataSafetyLog("saveData " + key + " refused: not a JSON object (" + why + ", " + std::to_string(text.size()) +
                          " bytes); " + key + ".json is left as it is");
        } else {
            const fs::path path = DataDir() / (Wide(key) + L".json");
            if (key == "anilist" || key == "prefs") {
                bool rotated = false;
                saved = WriteFileUtf8WithBackup(path, text, BackupRotationDue(key), &rotated);
                if (rotated) {
                    g_backup_rotated[key] = std::chrono::steady_clock::now();
                    DataSafetyLog("backup: previous " + key + ".json kept as " + key + ".json.bak1 (.bak1-.bak3 rotated)");
                }
            } else {
                saved = WriteFileUtf8(path, text);
            }
            if (!saved) {
                reason = "write-failed";
                DataSafetyLog("saveData " + key + " failed: the file could not be written (error " + std::to_string(GetLastError()) + ")");
            }
        }
        if (saved && key == "progress" && g_lan_mode == "server") SaveLanProgress("default", text);
        if (message["reqId"].type() == vp::Json::Type::Number)
            PostToWeb(Reply("dataSaved", message, std::string("\"ok\":") + (saved ? "true" : "false") +
                            (*reason ? std::string(",\"reason\":") + vp::Json::Quote(reason) : std::string())));
    } else if (cmd == "probeMedia") {
        // Header-only FFmpeg probe for the detail page's file-format tag. No cache dir is passed,
        // so ProbeJson does not extract attached fonts or write anything.
        std::thread([message]() {
#ifndef VP_NO_REMUX
            const std::string json = vp::remux::ProbeJson(fs::path(Wide(message["path"].str())), fs::path());
#else
            const std::string json;
#endif
            PostToWebFromAnyThread(Reply("mediaProbe", message, "\"probe\":" + (json.empty() ? std::string("null") : json)));
        }).detach();
    } else if (cmd == "scan") {
        ScanLibraries(message);
    } else if (cmd == "pickFile" || cmd == "pickFolder" || cmd == "pickSubtitle") {
        const std::string path = PickPath(cmd == "pickFolder", cmd == "pickSubtitle");
        PostToWeb(Reply("picked", message, "\"path\":" + vp::Json::Quote(path)));
    } else if (cmd == "thumbnails") {
        StartThumbnails(message);
    } else if (cmd == "saveImage") {
        SaveImage(message);
    } else if (cmd == "artwork") {
        ExtractArtwork(message);
    } else if (cmd == "http" || cmd == "anilist") {
        HttpRequest(message);
    } else if (cmd == "thumbnailsAt") {
        StartThumbnailsAt(message);
    } else if (cmd == "cancelThumbnails") {
        ++ThumbCounter(LanSession(message), SlotIndex(message["slot"].str()));
    } else if (cmd == "listAsio") {
#if defined(VP_ENGINE_MPV)
        PostToWeb(Reply("asioDrivers", message, "\"drivers\":[]"));
#else
        ListAsioDrivers(message);
#endif
    } else if (cmd == "listWasapi") {
#if defined(VP_ENGINE_MPV)
        EnsurePlayer();
        const std::string devices = g.player && g.player->Ready() ? g.player->WasapiDevicesJson() : "[]";
        PostToWeb(Reply("wasapiDevices", message, "\"devices\":" + devices));
#else
        ListWasapiDevices(message);
#endif
    } else if (cmd == "restartAudio") {
#if !defined(VP_ENGINE_MPV)
        StartAudioHost();
#endif
        PostToWeb(Reply("audioStatus", message, AudioStatusJson()));
    } else if (cmd == "audioStatus") {
        PostToWeb(Reply("audioStatus", message, AudioStatusJson()));
    } else if (!kLanSharingAvailable && (cmd == "lanDiscover" || cmd == "lanConnect" || cmd == "lanNewPin" ||
                                         cmd == "lanRevoke" || cmd == "lanUsersSave")) {
        // 區網分享 is closed in 1.0.0: no discovery broadcast, no client switch, no LAN user edits.
        PostToWeb(Reply(cmd == "lanDiscover" ? "lanServers" : "lanInfo", message,
                        cmd == "lanDiscover" ? std::string("\"servers\":[]") : LanStatusJson()));
    } else if (cmd == "lanInfo") {
        PostToWeb(Reply("lanInfo", message, LanStatusJson()));
    } else if (cmd == "lanApply") {
        // The page saved config.json ("lan" section): start/stop sharing or switch client mode.
        const std::string before_mode = g_lan_mode, before_server = g_lan_server;
        ApplyLanConfig();
        PostToWeb(Reply("lanInfo", message, LanStatusJson()));
        if (before_mode != g_lan_mode || before_server != g_lan_server) {
#if defined(VP_ENGINE_MPV)
            if (g.player && g.player->Ready()) g.player->Stop();
#else
            if (g.player) pc_stop(g.player);
#endif
            ShowVideoSurface(false);
            NavigateToUi();
        }
    } else if (cmd == "lanNewPin") {
        g_lan.RegeneratePin();
        PostToWeb(Reply("lanInfo", message, LanStatusJson()));
    } else if (cmd == "lanRevoke") {
        g_lan.Revoke(message["id"].str());
        PostToWeb(Reply("lanInfo", message, LanStatusJson()));
    } else if (cmd == "lanUsersSave") {
        const bool saved = SaveLanUsers(message["users"].str(), message["assignments"].str());
        PostToWeb(Reply("lanInfo", message, LanStatusJson() + std::string(",\"saved\":") + (saved ? "true" : "false")));
    } else if (cmd == "lanDiscover") {
        std::thread([message]() {
            std::string list = "[";
            for (const auto& server : vp::lan::Discover(1500))
                list += std::string(list.size() > 1 ? "," : "") + "{\"name\":" + vp::Json::Quote(server.name) + ",\"id\":" + vp::Json::Quote(server.id) +
                        ",\"address\":" + vp::Json::Quote(server.address) + "}";
            PostToWebFromAnyThread(Reply("lanServers", message, "\"servers\":" + list + "]"));
        }).detach();
    } else if (cmd == "lanConnect" || cmd == "lanDisconnect") {
        // Client mode: remember (or forget) the server in config.json and reload the window.
        const std::string server = cmd == "lanConnect" ? NormalizeServer(message["server"].str()) : std::string();
        if (cmd == "lanConnect" && server.empty()) { PostToWeb(Reply("lanInfo", message, LanStatusJson())); return; }
        std::string text = LoadConfigText();
        bool valid = false;
        const vp::Json config = vp::Json::Parse(text, &valid);
        if (valid) {
            // Rewrite only the "lan" member; everything else is kept as written.
            const vp::Json& lan = config["lan"];
            const std::string section = "{\"mode\":\"client\",\"port\":" + std::to_string(lan["port"].i64(vp::lan::kDefaultHttpPort)) +
                                        ",\"name\":" + vp::Json::Quote(lan["name"].str()) + ",\"server\":" + vp::Json::Quote(server) + "}";
            const auto at = text.find("\"lan\"");
            if (at == std::string::npos) {
                const auto brace = text.find('{');
                text.insert(brace + 1, "\n  \"lan\": " + section + ",");
            } else {
                // Replace the object that follows "lan": (balanced braces, strings skipped).
                std::size_t start = text.find('{', at), end = start, depth = 0;
                bool in_string = false;
                for (; end < text.size(); ++end) {
                    const char c = text[end];
                    if (in_string) { if (c == '\\') ++end; else if (c == '"') in_string = false; continue; }
                    if (c == '"') in_string = true;
                    else if (c == '{') ++depth;
                    else if (c == '}' && --depth == 0) break;
                }
                if (start != std::string::npos && end < text.size()) text.replace(start, end - start + 1, section);
            }
            bool still_valid = false;
            vp::Json::Parse(text, &still_valid);
#if defined(VP_ENGINE_MPV)
            if (still_valid) WriteMpvConfigText(text); // text is the MPV view: "lan" is stored as "mpvLan"
#else
            if (still_valid) WriteFileUtf8(ConfigPath(), text);
#endif
        }
        ApplyLanConfig();
#if defined(VP_ENGINE_MPV)
        if (g.player && g.player->Ready()) g.player->Stop();
#else
        if (g.player) pc_stop(g.player);
#endif
        ShowVideoSurface(false);
        NavigateToUi();
    } else if (cmd == "devtools") {
        if (g.webview) g.webview->OpenDevToolsWindow();
    } else if (cmd == "quit") {
        PostMessageW(g.window, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------- composition + webview
bool CreateComposition() {
    HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&g.dcomp));
    if (FAILED(hr)) return false;
    hr = g.dcomp->CreateTargetForHwnd(g.window, TRUE, &g.target);
#if defined(VP_FAULT_INJECTION)
    if (SUCCEEDED(hr) && FaultEnv(L"VP_TEST_FAIL_COMPOSITION")) hr = E_FAIL; // 2A test: early return 2 with a partial DComp tree
#endif
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(&g.root_visual);
#if !defined(VP_ENGINE_MPV)
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(&g.video_visual);
#endif
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(&g.web_visual);
    if (FAILED(hr)) return false;
    g.target->SetRoot(g.root_visual.Get());
    // WebView on top; the video goes directly underneath it (explicit reference visual).
    g.root_visual->AddVisual(g.web_visual.Get(), FALSE, nullptr);
#if !defined(VP_ENGINE_MPV)
    g.root_visual->AddVisual(g.video_visual.Get(), FALSE, g.web_visual.Get());
#endif
    return SUCCEEDED(g.dcomp->Commit());
}

void FailStartup(const wchar_t* what, HRESULT hr) {
    if (g.closing || !IsWindow(g.window)) { // 2A: a late WebView2 failure during exit: no MessageBox, no WM_CLOSE to a dead window
        AppendHostLogDirect("webview2 startup failure after close suppressed (hr=0x" + [hr] {
            char hex[16]{}; std::snprintf(hex, sizeof hex, "%08lX", static_cast<unsigned long>(hr)); return std::string(hex); }() + ")");
        return;
    }
    wchar_t text[512];
    std::swprintf(text, 512,
                  L"%ls (0x%08lx)\n\n請確認已安裝 Microsoft Edge WebView2 Runtime（Windows 11 內建）。", what, static_cast<unsigned long>(hr));
    MessageBoxW(g.window, text, L"SceneArc", MB_ICONERROR);
    PostMessageW(g.window, WM_CLOSE, 0, 0);
}

void CreateWebView() {
#if defined(VP_ENGINE_MPV)
    const std::wstring user_data = (DataDir() / L"webview2-mpv").wstring();
#else
    const std::wstring user_data = (DataDir() / L"webview2").wstring();
#endif
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (g.closing || !IsWindow(g.window)) { // 2A: closed before the environment arrived
                AppendHostLogDirect("webview2 environment completed after close; ignored");
                return S_OK;
            }
            if (FAILED(result) || !environment) { FailStartup(L"WebView2 環境建立失敗", result); return S_OK; }
            g.environment = environment;
            ComPtr<ICoreWebView2Environment3> environment3;
            if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&environment3)))) { FailStartup(L"WebView2 版本過舊（需要 visual hosting）", E_NOINTERFACE); return S_OK; }
            environment3->CreateCoreWebView2CompositionController(
                g.window,
                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [](HRESULT result, ICoreWebView2CompositionController* composition) -> HRESULT {
                        if (g.closing || !IsWindow(g.window)) {
                            // 2A: closed while the controller was being created: close the late one at once, never store it.
                            ComPtr<ICoreWebView2Controller> late;
                            if (composition && SUCCEEDED(composition->QueryInterface(IID_PPV_ARGS(&late)))) late->Close();
                            AppendHostLogDirect(std::string("webview2 controller completed after close; ") + (late ? "closed at once" : "none to close"));
                            return S_OK;
                        }
                        if (FAILED(result) || !composition) { FailStartup(L"WebView2 composition controller 建立失敗", result); return S_OK; }
                        g.composition = composition;
                        g.composition->QueryInterface(IID_PPV_ARGS(&g.controller));
                        g.controller->get_CoreWebView2(&g.webview);
                        g.composition->put_RootVisualTarget(g.web_visual.Get());

                        ComPtr<ICoreWebView2Controller2> controller2;
                        if (SUCCEEDED(g.controller.As(&controller2))) controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});

                        ComPtr<ICoreWebView2Settings> settings;
                        g.webview->get_Settings(&settings);
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE); // 1.0.0: no DevTools (F12, Ctrl+Shift+I or the devtools message)
                        settings->put_IsZoomControlEnabled(FALSE);

                        ComPtr<ICoreWebView2_3> webview3;
                        if (SUCCEEDED(g.webview.As(&webview3))) {
                            std::error_code ec;
                            fs::create_directories(DataDir() / L"thumbs", ec);
                            fs::create_directories(DataDir() / L"cache", ec);
                            wchar_t windows[MAX_PATH]{};
                            GetWindowsDirectoryW(windows, MAX_PATH);
                            const auto allow = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW;
                            webview3->SetVirtualHostNameToFolderMapping(L"app.local", (g.root / L"ui").c_str(), allow);
                            webview3->SetVirtualHostNameToFolderMapping(L"thumbs.local", (DataDir() / L"thumbs").c_str(), allow);
                            webview3->SetVirtualHostNameToFolderMapping(L"cache.local", (DataDir() / L"cache").c_str(), allow);
                            // System fonts for the ASS renderer (fallback CJK fonts, fonts named by scripts).
                            webview3->SetVirtualHostNameToFolderMapping(L"fonts.local", (fs::path(windows) / L"Fonts").c_str(), allow);
                        }

                        EventRegistrationToken token{};
                        g.webview->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    LPWSTR json = nullptr;
                                    LPWSTR source = nullptr;
                                    args->get_Source(&source);
                                    const std::string origin = source ? Utf8(source) : std::string();
                                    if (source) CoTaskMemFree(source);
                                    if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                                        // The local UI has full access; the LAN server's page (client
                                        // mode) only controls playback; anything else is ignored.
                                        if (StartsWith(origin, "https://app.local/")) HandleWebMessage(Utf8(json));
                                        else if (g_lan_mode == "client" && !g_lan_server.empty() && StartsWith(origin, g_lan_server + "/"))
                                            HandleServerPageMessage(Utf8(json));
                                        CoTaskMemFree(json);
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        g.webview->AddWebResourceRequestedFilter(L"https://media.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        g.webview->add_WebResourceRequested(
                            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                    ServeMediaFile(args);
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        g.composition->add_CursorChanged(
                            Callback<ICoreWebView2CursorChangedEventHandler>([](ICoreWebView2CompositionController* sender, IUnknown*) -> HRESULT {
                                // A null cursor is the page's `cursor: none` (the player hides the pointer while idle): apply it too.
                                HCURSOR cursor = nullptr;
                                if (SUCCEEDED(sender->get_Cursor(&cursor))) SetCursor(cursor);
                                return S_OK;
                            }).Get(),
                            &token);

                        OnResize();
                        g.controller->put_IsVisible(TRUE);
                        g.webview->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                    BOOL success = TRUE;
                                    args->get_IsSuccess(&success);
                                    LPWSTR uri = nullptr;
                                    sender->get_Source(&uri);
                                    const std::string page = uri ? Utf8(uri) : std::string();
                                    if (uri) CoTaskMemFree(uri);
                                    // The LAN server is offline: show the local connect page instead.
                                    if (!success && g_lan_mode == "client" && !g_lan_server.empty() && StartsWith(page, g_lan_server + "/"))
                                        sender->Navigate(L"https://app.local/index.html?connect=1&offline=1");
#if defined(VP_ENGINE_MPV)
                                    // Load libmpv and bring up its idle VO (D3D11 device) now, not on
                                    // the UI thread at the first `open`. Posted so this callback returns first.
                                    static bool warm_posted = false;
                                    if (success && !warm_posted) {
                                        warm_posted = true;
                                        PostMessageW(g.window, WM_APP_WARM_PLAYER, 0, 0);
                                    }
#endif
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        NavigateToUi();
                        g.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                        if (g.dcomp) g.dcomp->Commit();
                        return S_OK;
                    })
                    .Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) FailStartup(L"找不到 WebView2 Runtime", hr);
}

// Visual hosting: the host forwards mouse input to the WebView (keyboard goes to its focus window).
bool ForwardMouse(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!g.composition) return false;
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ScreenToClient(g.window, &point); // wheel coordinates are screen based
    if (message == WM_MOUSEMOVE && !g.tracking_mouse) {
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, g.window, 0};
        TrackMouseEvent(&track);
        g.tracking_mouse = true;
    }
    if (message == WM_MOUSELEAVE) g.tracking_mouse = false;
    switch (message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        SetCapture(g.window);
        if (g.controller) g.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        ReleaseCapture();
        break;
    default: break;
    }
    UINT32 mouse_data = 0;
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) mouse_data = static_cast<UINT32>(static_cast<INT32>(GET_WHEEL_DELTA_WPARAM(wparam)));
    if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) mouse_data = GET_XBUTTON_WPARAM(wparam);
    const auto keys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(message == WM_MOUSELEAVE ? 0 : GET_KEYSTATE_WPARAM(wparam));
    g.composition->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(message), keys, mouse_data,
                                  message == WM_MOUSELEAVE ? POINT{0, 0} : point);
    return true;
}

#if defined(VP_ENGINE_MPV)
LRESULT CALLBACK MpvChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
#endif

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) OnResize();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested && !IsZoomed(window))
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        OnResize();
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        if (VideoSurfaceInUse()) {
            FillRect(reinterpret_cast<HDC>(wparam), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        } else {
            HBRUSH brush = CreateSolidBrush(RGB(232, 237, 244)); // the UI's --ground
            FillRect(reinterpret_cast<HDC>(wparam), &rect, brush);
            DeleteObject(brush);
        }
        return 1;
    }
    case WM_SETFOCUS:
        if (g.controller) g.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_MOVE: case WM_MOVING:
        if (g.controller) g.controller->NotifyParentWindowPositionChanged();
#if defined(VP_ENGINE_MPV)
        // HDR output: the display under the window may have changed (re-read on the next frameProcessing tick).
        if (g.player) g.player->InvalidateDisplayCaps();
#endif
        return 0;
#if defined(VP_ENGINE_MPV)
    case WM_DISPLAYCHANGE: // mode / Windows HDR changes: re-read the display's HDR state
        if (g.player) g.player->InvalidateDisplayCaps();
        break;
#endif
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && g.composition) {
            HCURSOR cursor = nullptr;
            if (SUCCEEDED(g.composition->get_Cursor(&cursor))) { SetCursor(cursor); return TRUE; } // null = `cursor: none`
        }
        break;
    case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        if (ForwardMouse(message, wparam, lparam)) return message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK ? TRUE : 0;
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {960, 600};
        return 0;
    }
#if !defined(VP_ENGINE_MPV)
    case WM_APP_PLAYER: {
        std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lparam));
        HandlePlayerEvent(static_cast<int>(wparam), *json);
        return 0;
    }
#else
    case WM_APP_MPV_WAKE:
        if (g.player && g.player->Ready()) g.player->PollEvents();
        return 0;
    case WM_APP_WARM_PLAYER:
        EnsurePlayer();
        return 0;
#endif
    case WM_APP_POST: {
        std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lparam));
        PostToWeb(*json);
        return 0;
    }
    case WM_APP_RUN: {
        std::unique_ptr<std::function<void()>> work(reinterpret_cast<std::function<void()>*>(lparam));
        if (work && *work) (*work)();
        return 0;
    }
    case WM_APP_LAN: {
        std::unique_ptr<LanMessage> lan(reinterpret_cast<LanMessage*>(lparam));
        if (lan->kind == 0) HandleLanRpc(lan->session, lan->user_id, lan->text);
        else if (lan->kind == 1) {
#if defined(VP_ENGINE_MPV)
            if (g.player && g.player->Ready()) g.player->LoadSubtitle(lan->text);
#else
            if (g.player) pc_load_subtitle_file(g.player, lan->text.c_str());
#endif
        }
        return 0;
    }

    case WM_CLOSE:
        g.closing = true; // 2A: before anything else, so a WebView2 callback dispatched from here on is ignored
#if defined(VP_ENGINE_MPV)
        // The window goes away at once. With a VapourSynth chain initialising (TensorRT engine build), libmpv can only
        // quit once the script load returns; that wait is bounded and happens off screen (WM_DESTROY, wWinMain).
        if (g.player && g.player->VsBusy()) g.player->HostLog("close requested during VapourSynth init; hiding the window");
        ShowWindow(window, SW_HIDE);
#endif
        g_lan.Stop();
#if !defined(VP_ENGINE_MPV)
        if (g.player) { pc_destroy(g.player); g.player = nullptr; }
#endif
        ReleaseWebView(); // 2A: Close() + release on this STA (was: Close() only; the release ran after CoUninitialize)
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
#if defined(VP_ENGINE_MPV)
        // libmpv's shutdown runs on a worker. A normal one (~0.2 s) is waited for here, so mpv's video window goes
        // before its parent; one held by a VapourSynth init (TensorRT engine build: minutes) is not waited for past 2 s:
        // wWinMain waits for the rest of the 10 s after the message loop, when no window is left to look hung.
        if (g.player) {
            g.player->BeginShutdown();
            if (!g.player->AwaitShutdown(2000)) g.player->HostLog("libmpv still shutting down after 2000 ms; leaving the message loop");
        }
#else
        StopAudioHost();
#endif
        ReleaseComposition(); // 2A: the DirectComposition target goes while its window still exists
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// ---------------------------------------------------------------- single instance (1B-b H3)
// One process per data folder: two instances on the same data\ would each hold their own in-memory copy of
// anilist / prefs / progress and overwrite each other's saves. The mutex name carries a hash of the data folder path
// (case-folded), so a second install, a test copy or a worktree build with its own data\ is not affected. The main
// window carries the same value as a window property, so the second process fronts only its own first instance.
constexpr wchar_t kInstanceProp[] = L"VideoPlayer.DataDirKey";

std::uint64_t DataDirKey() {
    std::wstring path = DataDir().lexically_normal().wstring();
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a 64 over the UTF-16 code units
    for (const wchar_t ch : path) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

std::wstring InstanceMutexName(std::uint64_t key) {
    wchar_t name[96]{};
#if defined(VP_ENGINE_MPV)
    swprintf_s(name, std::size(name), L"Local\\VideoPlayer-mpv-%016llx", static_cast<unsigned long long>(key));
#else
    swprintf_s(name, std::size(name), L"Local\\VideoPlayer-pc-%016llx", static_cast<unsigned long long>(key));
#endif
    return name;
}

// The second process: find the first instance's main window (class VideoPlayerHost with our data-folder key; it may
// still be starting, so up to 3 s), restore it if minimized and bring it to the front. Touches no file.
bool FrontFirstInstance(std::uint64_t key) {
    struct Search { HANDLE want; HWND found; } search{reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(key)), nullptr};
    for (int attempt = 0; attempt < 30 && !search.found; ++attempt) {
        if (attempt) Sleep(100);
        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            wchar_t cls[64]{};
            GetClassNameW(window, cls, static_cast<int>(std::size(cls)));
            if (std::wcscmp(cls, kClassName) != 0 || GetPropW(window, kInstanceProp) != s->want) return TRUE;
            s->found = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
    }
    if (!search.found) return false;
    if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
    SetForegroundWindow(search.found);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
#if defined(VP_ENGINE_MPV)
    // MPV's optional VapourSynth/Python modules use the default DLL search path.
    // Establish it before WebView2 loads any of its own runtime DLLs.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    // VapourSynth autoloads every DLL in vs-runtime\vs-plugins. A damaged one (e.g. a half-copied vstrt.dll) raised a
    // system "映像錯誤" (bad image) dialog from LoadLibrary that blocked mpv's playback thread, and with it this UI
    // thread, until dismissed (measured with a 0-byte vstrt.dll). With critical-error boxes off, the load just fails
    // and VapourSynth skips that plugin.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Core-reliability 2A: every way out of wWinMain, the early returns included, releases this STA's WebView2 and
    // DirectComposition objects first and then balances a successful CoInitializeEx exactly once.
    struct ComApartmentScope {
        HRESULT init;
        bool finished = false;
        void Finish() {
            if (finished) return;
            finished = true;
            ReleaseWebView();
            ReleaseComposition();
            if (SUCCEEDED(init)) CoUninitialize();
        }
        ~ComApartmentScope() { Finish(); }
    } com_scope{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    g.root = FindAppRoot();
    // 1B-b H3: before anything below writes into data\ (crash tripwire folders, config, WebView2 profile, saves). The
    // handle stays open for the life of the process; Windows releases the mutex when the process ends, also on a crash.
    const std::uint64_t data_key = DataDirKey();
    const HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, InstanceMutexName(data_key).c_str());
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        const bool fronted = FrontFirstInstance(data_key);
#if defined(VP_ENGINE_MPV)
        // One line in the running instance's mpv-host.log (appended, shared); only when data\logs already exists.
        g_crash_host_log = (DataDir() / L"logs" / L"mpv-host.log").wstring();
        if (fs::is_directory(DataDir() / L"logs"))
            AppendHostLogDirect("second instance pid=" + std::to_string(GetCurrentProcessId()) + " exited: this data folder is in use by a running instance" +
                                (fronted ? " (brought to front)" : " (its window was not found)"));
        g_crash_host_log.clear();
#endif
        CloseHandle(instance_mutex);
        return 0;
    }
#if defined(VP_ENGINE_MPV)
    InstallCrashTripwire();
#endif

    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW)};
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    window_class.lpszClassName = kClassName;
    RegisterClassExW(&window_class);

#if defined(VP_ENGINE_MPV)
    WNDCLASSEXW mpv_class{sizeof(WNDCLASSEXW)};
    mpv_class.lpfnWndProc = MpvChildProc;
    mpv_class.hInstance = instance;
    mpv_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    mpv_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    mpv_class.lpszClassName = kMpvChildClass;
    RegisterClassExW(&mpv_class);
#endif

    const int width = 1600, height = 900;
    // WS_CLIPCHILDREN: the main window's erase never paints over the mpv video child.
    g.window = CreateWindowExW(0, kClassName, L"SceneArc", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr,
                               nullptr, instance, nullptr);
    if (!g.window) return 1;
    SetPropW(g.window, kInstanceProp, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(data_key))); // 1B-b H3
#if defined(VP_ENGINE_MPV)
    g.video_child = CreateWindowExW(0, kMpvChildClass, L"", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS,
                                    0, 0, 1, 1, g.window, nullptr, instance, nullptr);
#if defined(VP_FAULT_INJECTION)
    if (FaultEnv(L"VP_TEST_FAIL_CHILD")) { DestroyWindow(g.video_child); g.video_child = nullptr; } // 2A test: early return 1
#endif
    if (!g.video_child) return 1;
#endif
    if (!CreateComposition()) {
        MessageBoxW(g.window, L"DirectComposition 初始化失敗", L"SceneArc", MB_ICONERROR);
        return 2;
    }
    (void)show;
    ShowWindow(g.window, SW_SHOWMAXIMIZED);
    UpdateWindow(g.window);

    LoadConfigText(); // writes a default config.json on first run
#if defined(VP_ENGINE_MPV)
    if (!EnsureMpvConfigKeys())
        MessageBoxW(g.window, L"無法安全備份或更新 MPV 設定。原有設定未覆寫；請檢查 config.json 與備份檔。", L"SceneArc", MB_ICONWARNING);
#endif
    if (vp::Json::Parse(LoadConfigText())["startFullscreen"].boolean()) SetFullscreen(true); // "啟動時全螢幕"
    ApplyLanConfig();
#if !defined(VP_ENGINE_MPV)
    StartAudioHost();
#endif
    CreateWebView();

MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ReleaseWebView();     // 2A: idempotent; covers a DestroyWindow that did not come through WM_CLOSE
    ReleaseComposition();
#if defined(VP_ENGINE_MPV)
    bool libmpv_still_running = false;
    if (g.player) { g.player->Shutdown(); libmpv_still_running = g.player->ShutdownDetached(); g.player.reset(); }
#endif
    if (g.audio_job) CloseHandle(g.audio_job);
    com_scope.Finish(); // CoUninitialize after the STA objects are gone (was: before the static destructor released them)
#if defined(VP_ENGINE_MPV)
    if (libmpv_still_running) {
        // 2A: the exceptional path only. mpv_terminate_destroy is still inside a VapourSynth/TensorRT init after the 10 s
        // bound; a normal ExitProcess would run DLL_PROCESS_DETACH of CUDA/TensorRT/Python with those threads killed
        // mid-call. Everything this process owns is already released; end it without the DLL detach.
        AppendHostLogDirect("process exit via TerminateProcess: libmpv shutdown still running after the 10000 ms bound");
        TerminateProcess(GetCurrentProcess(), 0);
    }
#endif
    return 0;
}
