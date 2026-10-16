#include "mpv_backend.h"

#include "json.h"

#include <mpv/client.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace vp {
namespace {

std::atomic<bool> g_mpv_process_initialization_claimed{};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

const mpv_node* MapValue(const mpv_node* node, const char* key) {
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list) return nullptr;
    const mpv_node_list* list = node->u.list;
    for (int i = 0; i < list->num; ++i)
        if (list->keys[i] && std::string_view(list->keys[i]) == key) return &list->values[i];
    return nullptr;
}

std::string NodeString(const mpv_node* node, const char* fallback = "") {
    return node && node->format == MPV_FORMAT_STRING && node->u.string ? node->u.string : fallback;
}

double NodeNumber(const mpv_node* node, double fallback = 0.0) {
    if (!node) return fallback;
    if (node->format == MPV_FORMAT_DOUBLE) return node->u.double_;
    if (node->format == MPV_FORMAT_INT64) return static_cast<double>(node->u.int64);
    if (node->format == MPV_FORMAT_FLAG) return node->u.flag ? 1.0 : 0.0;
    return fallback;
}

bool NodeBool(const mpv_node* node) { return NodeNumber(node) != 0.0; }

// Generated VapourSynth scripts are named by a hash of every substituted value, and the
// enhancement pass runs twice per file (on_preloaded, then FILE_LOADED). Rewriting an
// identical file could truncate it while mpv's vapoursynth filter is reading it, so an
// unchanged script is left alone.
bool WriteFileIfDifferent(const std::filesystem::path& path, const std::string& content) {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            const std::string current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current == content) return true;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output && (output << content);
}

double MsSince(std::chrono::steady_clock::time_point start) {
    if (start.time_since_epoch().count() == 0) return 0.0;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string Number(double value, int precision = 3) {
    if (!std::isfinite(value)) return "0";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    std::string text = out.str();
    while (text.find('.') != std::string::npos && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text.empty() ? "0" : text;
}

// Per-file option ",sub-fonts-dir=%len%<dir>" for a local file with a sibling fonts/Fonts/attachments
// folder (release-group font packs); empty for streams or when no such folder exists.
// sub-fonts-dir holds one path, so a per-video folder replaces data\fonts for that file only.
std::string LocalSubFontsDirOption(const std::string& path) {
    if (path.empty() || path.find("://") != std::string::npos) return {};
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
    if (dir.empty()) return {};
    for (const wchar_t* name : {L"fonts", L"Fonts", L"attachments"}) {
        if (!std::filesystem::is_directory(dir / name, ec)) continue;
        const std::string folder = Utf8((dir / name).wstring());
        // mpv %len% quoting: the path may contain ',' or '='.
        return ",sub-fonts-dir=%" + std::to_string(folder.size()) + "%" + folder;
    }
    return {};
}

std::string MpvErrorText(decltype(&mpv_error_string) error_string, int code) {
    const char* text = error_string ? error_string(code) : nullptr;
    return text ? text : "unknown libmpv error";
}

std::string GpuVendorName(std::uint32_t vendor_id) {
    if (vendor_id == 0x1002) return "amd";
    if (vendor_id == 0x10DE) return "nvidia";
    if (vendor_id == 0x8086) return "intel";
    return "any";
}

// hwdec for the renderer API. Vulkan cannot attach d3d11va (mpv: "Could not create device" -> silent software
// decoding), so the D3D11 defaults map to Vulkan Video (zero-copy; measured on RX 9060 XT) and the copy-back
// variant maps to auto-copy, which resolves to d3d11va-copy under Vulkan. vulkan-copy is never chosen: it
// fails with decode errors on the AMD driver. Other explicit values pass through.
std::string ResolveHwdec(const std::string& gpu_api, const std::string& configured) {
    if (gpu_api == "vulkan") {
        if (configured.empty() || configured == "d3d11va") return "vulkan";
        if (configured == "d3d11va-copy") return "auto-copy";
        return configured;
    }
    return configured.empty() ? std::string("d3d11va") : configured;
}

int MonitorHeight(HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    return std::max(1L, info.rcMonitor.bottom - info.rcMonitor.top);
}

// HDR source class from video-params: the transfer (pq / hlg) decides; a tagged peak above SDR white (sig-peak > 1, the
// key mpv-lazy's [HDR_generic] profile uses) is the secondary indicator. Never the bit depth: 10-bit SDR (S1-01, EVA
// BT.2020) is SDR. "unknown" while video-params is not available (on_preloaded, before the decoder's first frame).
std::string HdrSourceClass(const std::string& gamma, double sig_peak) {
    const std::string transfer = Lower(gamma);
    if (transfer == "pq" || transfer == "hlg") return transfer;
    if (sig_peak > 1.0) return "hdr";
    return transfer.empty() ? std::string("unknown") : std::string("sdr");
}

bool IsHdrSourceClass(const std::string& source) { return source == "pq" || source == "hlg" || source == "hdr"; }

const char* DxgiColorSpaceName(DXGI_COLOR_SPACE_TYPE space) {
    switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "RGB_FULL_G22_NONE_P2020";
    default: return "other";
    }
}

// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2+; the SDK declares it only for NTDDI_WIN11_GA, this build
// targets 0x0A00): same layout, type 15. Older Windows answers with an error and the v1 query is used instead.
struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value; // bit 0 advancedColorSupported, 1 advancedColorActive, 4 highDynamicRangeSupported, 5 HDR user-enabled
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
};

struct DisplayHdrQuery {
    bool known{};
    bool hdr_enabled{};
    bool capable_known{};
    bool hdr_capable{};
    double max_nits{};
    std::string device;
    std::string colorspace;
};

// Read-only display facts for the monitor under the player window (no system setting is touched):
//   DXGI IDXGIOutput6::GetDesc1 -> desktop colour space (G2084/P2020 = Windows HDR on) and MaxLuminance;
//   DisplayConfigGetDeviceInfo  -> whether the display can do HDR at all.
DisplayHdrQuery QueryDisplayHdr(HMONITOR monitor) {
    DisplayHdrQuery result;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return result;
    result.device = Utf8(info.szDevice);
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        for (UINT a = 0; !result.known; ++a) {
            IDXGIAdapter1* adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(a, &adapter)) || !adapter) break;
            for (UINT o = 0; !result.known; ++o) {
                IDXGIOutput* output = nullptr;
                if (FAILED(adapter->EnumOutputs(o, &output)) || !output) break;
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                    IDXGIOutput6* output6 = nullptr;
                    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) && output6) {
                        DXGI_OUTPUT_DESC1 desc1{};
                        if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                            result.known = true;
                            result.hdr_enabled = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                            result.max_nits = desc1.MaxLuminance;
                            result.colorspace = DxgiColorSpaceName(desc1.ColorSpace);
                        }
                        output6->Release();
                    }
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
        return result;
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0)
            continue;
        AdvancedColorInfo2 color2{};
        color2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15); // DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
        color2.header.size = sizeof(color2);
        color2.header.adapterId = paths[i].targetInfo.adapterId;
        color2.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS) {
            result.capable_known = true;
            result.hdr_capable = (color2.value & (1u << 4)) != 0;
        } else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
            color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            color.header.size = sizeof(color);
            color.header.adapterId = paths[i].targetInfo.adapterId;
            color.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
                result.capable_known = true;
                result.hdr_capable = color.advancedColorSupported != 0;
            }
        }
        break;
    }
    return result;
}

std::string ReplaceAll(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) return text;
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

std::uint64_t StableHash64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct FpsFactor {
    int num{2};
    int den{1};
    double value{2.0};
};

FpsFactor SelectAutoFpsFactor(double source_fps, double display_hz) {
    constexpr FpsFactor candidates[] = {{2, 1, 2.0}, {5, 2, 2.5}, {3, 1, 3.0}, {4, 1, 4.0}, {5, 1, 5.0}};
    if (!(source_fps > 0.0) || !(display_hz > 0.0)) return {};
    for (const auto& candidate : candidates) {
        const double output_fps = source_fps * candidate.value;
        // Prefer a cadence at or above half refresh; this matches the selected 24p/120Hz,
        // 24p/144Hz, 24p/60Hz and 30p/120Hz examples while avoiding needless lower multiples.
        if (output_fps + 0.1 < display_hz * 0.5 || output_fps > display_hz + 0.01) continue;
        const double repeats = display_hz / output_fps;
        if (std::abs(repeats - std::round(repeats)) <= 0.01) return candidate;
    }
    return {}; // ×2 plus display-resample handles source rates without a reasonable exact cadence.
}

FpsFactor ParseFpsFactor(const std::string& value, double source_fps, double display_hz) {
    if (value == "auto" || value.empty()) return SelectAutoFpsFactor(source_fps, display_hz);
    if (value == "2.5") return {5, 2, 2.5};
    if (value == "3") return {3, 1, 3.0};
    if (value == "4") return {4, 1, 4.0};
    if (value == "5") return {5, 1, 5.0};
    return {2, 1, 2.0};
}

// __TRT_LOG__ in the TensorRT templates: <data_dir>/logs/vs-trt.log as a Python string literal. Forward slashes, and
// JSON string quoting (", \\ and control characters escaped; UTF-8 kept), which is also a valid Python literal.
std::string TrtLogLiteral(const std::filesystem::path& data_dir) {
    std::error_code ec;
    std::filesystem::create_directories(data_dir / L"logs", ec);
    std::string path = Utf8((data_dir / L"logs" / L"vs-trt.log").wstring());
    std::replace(path.begin(), path.end(), '\\', '/');
    return Json::Quote(path);
}

std::string VapourSynthGeneratedPath(const std::string& filter) {
    const auto marker = filter.find("file=%");
    if (marker == std::string::npos) return {};
    const auto digits_begin = marker + 6;
    const auto length_end = filter.find('%', digits_begin);
    if (length_end == std::string::npos || length_end == digits_begin) return {};
    std::size_t length = 0;
    try { length = static_cast<std::size_t>(std::stoull(filter.substr(digits_begin, length_end - digits_begin))); }
    catch (...) { return {}; }
    return filter.substr(length_end + 1, length);
}

// Every generated VapourSynth script starts with this line, and the cache name hashes the schema too. Schema 2 added
// the one-script upscale + interpolation chain (chain_<hash>.vpy) and the template sections; scripts from an older
// build (no such first line) are never reused and are removed from the cache once per process.
constexpr std::string_view kVsScriptSchemaLine = "# vp-generated-script schema=2";

// A VS template is one complete single-stage script split by marker lines: "# --- header ---" (imports, threads,
// clip = video_in, the TensorRT prologue), "# --- body ---" (the stage), "# --- footer ---" (clip.set_output()).
// header keeps its marker line; body and footer start after theirs. Expects \n line ends.
struct VsSections {
    std::string header;
    std::string body;
    std::string footer;
};

bool SplitVsSections(const std::string& script, VsSections* out) {
    const auto line_at = [&](std::string_view marker) -> std::size_t {
        if (script.compare(0, marker.size(), marker) == 0) return 0;
        const auto at = script.find("\n" + std::string(marker));
        return at == std::string::npos ? std::string::npos : at + 1;
    };
    const auto after_line = [&](std::size_t at) {
        const auto newline = script.find('\n', at);
        return newline == std::string::npos ? script.size() : newline + 1;
    };
    const auto body = line_at("# --- body ---");
    const auto footer = line_at("# --- footer ---");
    if (!out || body == std::string::npos || footer == std::string::npos || footer < after_line(body)) return false;
    out->header = script.substr(0, body);
    out->body = script.substr(after_line(body), footer - after_line(body));
    out->footer = script.substr(after_line(footer));
    return !out->header.empty() && !out->body.empty() && out->footer.find("set_output") != std::string::npos;
}

// upscaleParams.preHeight "auto" (mpv_PlayKit MIX_UAI_*: H_Pre 720 for a 1440 output with a x2 model): the smallest
// pre-scale height that still reaches the output cap after the model's scale, never above the source (FMT_CTRL only
// shrinks). Unknown source height (0): the cap-derived value. Unknown cap: 0 (no pre-scale).
int AutoUpscalePreHeight(int source_height, int max_height, int model_scale) {
    if (max_height <= 0) return 0;
    const int scale = std::max(1, model_scale);
    const int needed = (max_height + scale - 1) / scale;
    return source_height > 0 ? std::min(source_height, needed) : needed;
}

bool HasTrackType(mpv_handle* ctx, decltype(&mpv_get_property) get_property,
                  decltype(&mpv_free_node_contents) free_node_contents, const char* wanted) {
    if (!ctx || !get_property || !free_node_contents) return false;
    mpv_node value{};
    if (get_property(ctx, "track-list", MPV_FORMAT_NODE, &value) < 0) return false;
    bool found = false;
    if (value.format == MPV_FORMAT_NODE_ARRAY && value.u.list) {
        for (int i = 0; i < value.u.list->num; ++i) {
            const mpv_node* item = &value.u.list->values[i];
            if (NodeString(MapValue(item, "type")) == wanted) { found = true; break; }
        }
    }
    free_node_contents(&value);
    return found;
}

// Current refresh rate of the monitor showing `window`, from the display configuration
// (the same rational DWM uses), so the interpolation factor can be chosen before mpv's
// display-fps property is known. Falls back to the GDI mode's integer frequency.
double DisplayRefreshHz(HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0.0;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS && path_count > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < path_count; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = paths[i].sourceInfo.adapterId;
                source.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
                if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
                const auto rate = paths[i].targetInfo.refreshRate;
                if (rate.Denominator != 0 && rate.Numerator != 0)
                    return static_cast<double>(rate.Numerator) / static_cast<double>(rate.Denominator);
            }
        }
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return static_cast<double>(mode.dmDisplayFrequency);
    return 0.0;
}

constexpr std::uint64_t kPreloadHookId = 0x56505052454c4f41ull; // reply_userdata of the on_preloaded hook
constexpr UINT_PTR kFirstFrameTimerId = 0x5650;                 // SetTimer id on the host window
constexpr UINT kFirstFrameSafetyMs = 6000;
constexpr UINT_PTR kVsBusyTimerId = 0x5651;                     // 1 s tick while a VapourSynth chain initialises
constexpr auto kVsBusyTimeout = std::chrono::minutes(15);       // logged and reported, never cleared on its own
constexpr double kVsInterpolationMaxFps = 32.0;                  // k7sfunc FPS_CTRL fps_max (raises above it with fps_ret)
constexpr UINT_PTR kVsReconfigTimerId = 0x5652;                 // 100 ms reconfig-watch tick (audit 7.3b)
constexpr UINT kVsReconfigWatchMs = 100;
constexpr double kReconfigReloadObservedMs = 150.0;             // a watch probe that waited this long saw the reload
constexpr auto kReconfigFallback = std::chrono::seconds(3);     // no VIDEO_RECONFIG / no reload seen: end after this
constexpr int kVsDeadCleanupMax = 3;                            // dead-filter cleanups per file (audit 7.2c guard)
// k7sfunc 1.8.1 validates every AI stage's gpu argument as Literal[0, 1, 2] (_internal._validate_literal: UAI_DML /
// UAI_NV_TRT mod_mix.py, DRBA_HUB / RIFE_DML / RIFE_NV mod_memc.py) and raises vs.Error otherwise (audit 7.6d).
constexpr std::uint32_t kK7sfuncMaxGpuIndex = 2;
constexpr DWORD kShutdownWaitMs = 10000;                        // app exit: longest wait for mpv_terminate_destroy
constexpr std::uint64_t kReplyIdBase = 0x5650000000000000ull;   // async request ids (distinct from kPreloadHookId)
constexpr UINT_PTR kWakeWatchdogTimerId = 0x5653;               // 1B degraded mode only (no threadpool retry timer)
MpvBackend* g_timer_backend = nullptr; // one libmpv context per process (see Initialize)

std::int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if defined(VP_FAULT_INJECTION)
// Core-reliability test builds ONLY: compiled with /DVP_FAULT_INJECTION in a separate build directory; build-mpv.cmd,
// package-mpv.cmd and the CMake targets never define it, so the shipped VideoPlayer.exe has none of this.
//   VP_TEST_WAKE_FAIL    first:N | every:K | after:S,count:N | all   [,err:1816|1400]
//                        the matching wake posts fail with that Win32 error, without posting (1B, report 25-B T0-T8)
//   VP_TEST_WAKE_LEGACY=1  a failed post behaves as at HEAD 9202426 (token kept, no retry): reproduces the stall (T0)
//   VP_TEST_LOAD_REJECT=K  the K-th loadfile reply of the process (1-based) is treated as rejected if current (2B, T11)
//   VP_TEST_OPEN_BAD_OPTION=K  the K-th Open() appends a malformed per-file option (synchronous -4, 2B T12)
//   VP_TEST_OPEN_SLEEP_MS=N  Open() holds the UI thread N ms after submitting the loadfile (2B ordering tests)
//   VP_TEST_CHAIN_OPEN=<path>  after that hold, the first Open() submits a second Open(<path>) before any event is
//                          drained: the first file's END_FILE is then handled after the newer request began (2B T15);
//                          "<stop>" submits Stop() instead (a failure racing the user's stop, 2B T14/B5)
//   VP_TEST_FLOOD_QUEUE=1  Open() first posts WM_NULL to the host window until the queue quota fails (1B T2: a real
//                          ERROR_NOT_ENOUGH_QUOTA for mpv's wake post while the UI thread is held by OPEN_SLEEP)
struct FaultConfig {
    int wake_mode{};                 // 0 off | 1 first | 2 every | 3 after | 4 all
    long wake_n{};
    long wake_k{};
    std::int64_t wake_after_ns{};
    unsigned long wake_err{ERROR_NOT_ENOUGH_QUOTA};
    bool wake_legacy{};
    int load_reject{};
    int open_bad_option{};
    int open_sleep_ms{};
    bool flood_queue{};              // VP_TEST_FLOOD_QUEUE=1: fill the UI thread's posted-message queue before the loadfile
    std::string chain_open;
};
FaultConfig g_fault;
std::int64_t g_fault_start_ns{};
std::atomic<std::uint32_t> g_wake_attempts{}, g_wake_injected{}, g_wake_callbacks{}, g_wake_posts_ok{}, g_wake_posts_failed{},
    g_wake_retry_fires{}, g_drains{}, g_empty_drains{}, g_reentrant_drains{};
std::chrono::steady_clock::time_point g_wake_stats_at{};
std::vector<double> g_drain_ms; // UI thread only: drain durations since the last wake-stats line (T5 per-tick UI cost)
int g_loadfile_replies{};       // UI thread only
double g_fp_tick_acc{};         // UI thread only: request + reply handling of the snapshot in flight (T5 per-tick cost)
std::vector<double> g_fp_tick_ms;
int g_opens{};                  // UI thread only

std::string FaultEnv(const char* name) {
    char buffer[256]{};
    const DWORD n = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof buffer));
    return n > 0 && n < sizeof buffer ? std::string(buffer, n) : std::string();
}

std::string LoadFaultConfig() {
    const std::string wake = FaultEnv("VP_TEST_WAKE_FAIL");
    const auto value = [&](const char* key) -> long {
        const auto at = wake.find(key);
        return at == std::string::npos ? -1 : std::strtol(wake.c_str() + at + std::strlen(key), nullptr, 10);
    };
    if (wake.rfind("first:", 0) == 0) { g_fault.wake_mode = 1; g_fault.wake_n = value("first:"); }
    else if (wake.rfind("every:", 0) == 0) { g_fault.wake_mode = 2; g_fault.wake_k = value("every:"); }
    else if (wake.rfind("after:", 0) == 0) {
        g_fault.wake_mode = 3;
        g_fault.wake_after_ns = static_cast<std::int64_t>(value("after:")) * 1000000000ll;
        g_fault.wake_n = value("count:");
    } else if (wake.rfind("all", 0) == 0) g_fault.wake_mode = 4;
    if (value("err:") > 0) g_fault.wake_err = static_cast<unsigned long>(value("err:"));
    g_fault.wake_legacy = FaultEnv("VP_TEST_WAKE_LEGACY") == "1";
    g_fault.load_reject = std::atoi(FaultEnv("VP_TEST_LOAD_REJECT").c_str());
    g_fault.open_bad_option = std::atoi(FaultEnv("VP_TEST_OPEN_BAD_OPTION").c_str());
    g_fault.open_sleep_ms = std::atoi(FaultEnv("VP_TEST_OPEN_SLEEP_MS").c_str());
    g_fault.flood_queue = FaultEnv("VP_TEST_FLOOD_QUEUE") == "1";
    g_fault.chain_open = FaultEnv("VP_TEST_CHAIN_OPEN");
    g_fault_start_ns = SteadyNowNs();
    return "fault-injection build: VP_TEST_WAKE_FAIL=" + (wake.empty() ? std::string("-") : wake) +
           " mode=" + std::to_string(g_fault.wake_mode) + " err=" + std::to_string(g_fault.wake_err) +
           " legacy=" + (g_fault.wake_legacy ? "yes" : "no") + " load_reject=" + std::to_string(g_fault.load_reject) +
           " open_bad_option=" + std::to_string(g_fault.open_bad_option) +
           " open_sleep_ms=" + std::to_string(g_fault.open_sleep_ms) + " flood_queue=" + (g_fault.flood_queue ? "yes" : "no");
}

// mpv thread or threadpool thread: atomics only.
bool InjectWakeFailure(unsigned long* error) {
    if (!g_fault.wake_mode) return false;
    const std::uint32_t attempt = ++g_wake_attempts; // 1-based
    bool fail = false;
    switch (g_fault.wake_mode) {
    case 1: fail = static_cast<long>(attempt) <= g_fault.wake_n; break;
    case 2: fail = g_fault.wake_k > 0 && attempt % static_cast<std::uint32_t>(g_fault.wake_k) == 0; break;
    case 3: fail = SteadyNowNs() - g_fault_start_ns >= g_fault.wake_after_ns &&
                   static_cast<long>(g_wake_injected.load()) < g_fault.wake_n; break;
    case 4: fail = true; break;
    default: break;
    }
    if (!fail) return false;
    ++g_wake_injected;
    *error = g_fault.wake_err;
    return true;
}
#endif

// frameProcessing snapshot (see EmitFrameProcessing). Reply ids: kFpReplyBase + (generation << 8) + index.
constexpr std::uint64_t kFpReplyBase = 0x4650000000000000ull;
constexpr std::uint64_t kFpReplyRange = 1ull << 56;
// What the tick reads, with the format the synchronous read used: 'S' = PropertyString (MPV_FORMAT_STRING),
// 'D' = PropertyDouble (MPV_FORMAT_DOUBLE), 'N' = node (MPV_FORMAT_NODE). Served in this order on mpv's playback
// thread: the rate-window inputs first (time-pos's reply time is the sample time), vo-passes (a VO-thread round trip,
// up to one frame) last, so the others are read together. Observed properties are requested too: right after a file
// load or a change the observed cache can lag a read (measured: hwdec-current / container-fps / estimated-vf-fps /
// decoder-frame-drop-count still unset at the first tick while mpv_get_property had them).
struct FpRequest { char kind; const char* name; };
constexpr FpRequest kFpRequests[] = {
    {'D', "time-pos"}, {'D', "avsync"}, {'D', "frame-drop-count"}, {'D', "estimated-vf-fps"}, {'D', "container-fps"},
    {'D', "display-fps"}, {'D', "decoder-frame-drop-count"}, {'S', "hwdec-current"},
    {'N', "video-params"}, {'N', "video-out-params"}, {'N', "video-target-params"}, {'N', "audio-params"},
    {'N', "audio-out-params"}, {'S', "video-sync"}, {'D', "vo-delayed-frame-count"}, {'D', "video-bitrate"},
    {'D', "audio-bitrate"}, {'S', "current-vo"}, {'S', "current-gpu-context"}, {'D', "osd-width"}, {'D', "osd-height"},
    {'S', "audio-device"}, {'S', "current-ao"}, {'S', "audio-exclusive"}, {'D', "volume"}, {'S', "mute"}, {'S', "af"},
    {'D', "audio-speed-correction"}, {'N', "vo-passes"},
};
// Observed scalars the tick reads from the observed cache instead: display-names (observed for the tick only: mpv
// re-reads it on its window-state event instead of a VO-thread round trip every second; requested like the others
// while the cache has no value yet) and speed (cache-only before as well).
bool FpObservedScalar(const char* name) {
    return std::strcmp(name, "display-names") == 0 || std::strcmp(name, "speed") == 0;
}

} // namespace

// A deep copy of an mpv_node owned by this process: a GET_PROPERTY_REPLY's node is only valid until the next
// mpv_wait_event. Read with MapValue / NodeString / NodeNumber like libmpv's node; never passed to libmpv.
class OwnedNode {
public:
    explicit OwnedNode(const mpv_node& source) : root_(Copy(source)) {}
    OwnedNode(const OwnedNode&) = delete;
    OwnedNode& operator=(const OwnedNode&) = delete;
    const mpv_node* get() const { return &root_; }

private:
    char* Text(const char* text) {
        if (!text) return nullptr;
        strings_.emplace_back(text); // std::deque: element addresses stay valid
        return strings_.back().data();
    }
    mpv_node Copy(const mpv_node& in) {
        mpv_node out{};
        out.format = in.format;
        switch (in.format) {
        case MPV_FORMAT_STRING:
        case MPV_FORMAT_OSD_STRING: out.u.string = Text(in.u.string); break;
        case MPV_FORMAT_FLAG: out.u.flag = in.u.flag; break;
        case MPV_FORMAT_INT64: out.u.int64 = in.u.int64; break;
        case MPV_FORMAT_DOUBLE: out.u.double_ = in.u.double_; break;
        case MPV_FORMAT_NODE_ARRAY:
        case MPV_FORMAT_NODE_MAP: {
            const int count = in.u.list ? std::max(0, in.u.list->num) : 0;
            lists_.push_back(std::make_unique<mpv_node_list>());
            mpv_node_list* list = lists_.back().get();
            *list = {};
            values_.push_back(std::make_unique<mpv_node[]>(static_cast<std::size_t>(std::max(1, count))));
            list->values = values_.back().get();
            if (in.format == MPV_FORMAT_NODE_MAP) {
                keys_.push_back(std::make_unique<char*[]>(static_cast<std::size_t>(std::max(1, count))));
                list->keys = keys_.back().get();
            }
            for (int i = 0; i < count; ++i) {
                list->values[i] = Copy(in.u.list->values[i]);
                if (list->keys) list->keys[i] = Text(in.u.list->keys ? in.u.list->keys[i] : nullptr);
            }
            list->num = count;
            out.u.list = list;
            break;
        }
        default: out.format = MPV_FORMAT_NONE; break; // byte arrays are not read by the tick
        }
        return out;
    }
    std::deque<std::string> strings_;
    std::vector<std::unique_ptr<mpv_node_list>> lists_;
    std::vector<std::unique_ptr<mpv_node[]>> values_;
    std::vector<std::unique_ptr<char*[]>> keys_;
    mpv_node root_{};
};

// One node read for one caller: libmpv's node (freed here with mpv_free_node_contents) or a snapshot node (borrowed).
class NodeHolder {
public:
    explicit NodeHolder(void (*free_node)(mpv_node*)) : free_node_(free_node) {}
    ~NodeHolder() { if (owned_valid_ && free_node_) free_node_(&owned_); }
    NodeHolder(const NodeHolder&) = delete;
    NodeHolder& operator=(const NodeHolder&) = delete;
    mpv_node owned_{};
    bool owned_valid_{};

private:
    void (*free_node_)(mpv_node*);
};

bool TensorRtRuntimeInstalled(const std::filesystem::path& exe_dir, std::string* missing) {
    // vstrt.dll is the VapourSynth TRT filter (core.trt); vsmlrt.py runs vsmlrt-cuda/trtexec.exe to build engines and
    // vstrt preloads the TensorRT/CUDA DLLs from vsmlrt-cuda/ (mpv-lazy vsNV pack layout).
    const auto plugins = exe_dir / L"vs-runtime" / L"vs-plugins";
    std::error_code ec;
    const struct { std::filesystem::path path; bool directory; const char* name; } required[] = {
        {plugins / L"vstrt.dll", false, "vs-plugins\\vstrt.dll"},
        {plugins / L"vsmlrt-cuda", true, "vs-plugins\\vsmlrt-cuda\\"},
        {plugins / L"vsmlrt-cuda" / L"trtexec.exe", false, "vs-plugins\\vsmlrt-cuda\\trtexec.exe"},
    };
    for (const auto& item : required) {
        const bool present = item.directory ? std::filesystem::is_directory(item.path, ec) : std::filesystem::is_regular_file(item.path, ec);
        if (!present) {
            if (missing) *missing = item.name;
            return false;
        }
    }
    // vstrt preloads TensorRT (nvinfer_<major>.dll) and the CUDA runtime (cudart64_<major>.dll) from vsmlrt-cuda/, and
    // trtexec.exe resolves them from its own folder; names carry the major version, so match by prefix.
    const auto has_dll = [&](const std::wstring& prefix) {
        std::error_code it_ec;
        for (const auto& entry : std::filesystem::directory_iterator(plugins / L"vsmlrt-cuda", it_ec)) {
            if (it_ec) break;
            std::wstring name = entry.path().filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            std::error_code file_ec;
            if (name.size() >= prefix.size() + 4 &&name.compare(0, prefix.size(), prefix) == 0 &&
                name.compare(name.size() - 4, 4, L".dll") == 0 && entry.is_regular_file(file_ec))
                return true;
        }
        return false;
    };
    const struct { const wchar_t* prefix; const char* name; } dlls[] = {
        {L"nvinfer", "vs-plugins\\vsmlrt-cuda\\nvinfer*.dll"},
        {L"cudart64_", "vs-plugins\\vsmlrt-cuda\\cudart64_*.dll"},
    };
    for (const auto& dll : dlls) {
        if (!has_dll(dll.prefix)) {
            if (missing) *missing = dll.name;
            return false;
        }
    }
    if (missing) missing->clear();
    return true;
}

TensorRtModelExports TensorRtModelExportsPresent(const std::filesystem::path& exe_dir) {
    // DRBA_NV: drba/distilDRBA_{v1|v2_lite}[_scale]_ap.onnx (turbo 1 / turbo 2); RIFE_NV: rife_v2/ for fast, rife/ for
    // quality (v4.6 quality loads only the ensemble export). The RIFE exports are shared with DirectML.
    static const char* const kExports[] = {
        "drba/distilDRBA_v1_ap.onnx", "drba/distilDRBA_v2_lite_ap.onnx",
        "drba/distilDRBA_v1_scale_ap.onnx", "drba/distilDRBA_v2_lite_scale_ap.onnx",
        "rife_v2/rife_v4.6.onnx", "rife/rife_v4.6_ensemble.onnx",
    };
    const auto models = exe_dir / L"vs-runtime" / L"vs-plugins" / L"models";
    TensorRtModelExports result;
    for (const char* relative : kExports) {
        std::error_code ec;
        const bool present = std::filesystem::is_regular_file(models / std::filesystem::path(relative), ec);
        (present ? result.present : result.missing).emplace_back(relative);
    }
    return result;
}

struct MpvBackend::Api {
#define VP_MPV_FN(name) decltype(&mpv_##name) name{}
    VP_MPV_FN(create);
    VP_MPV_FN(initialize);
    VP_MPV_FN(terminate_destroy);
    VP_MPV_FN(set_option_string);
    VP_MPV_FN(set_property);
    VP_MPV_FN(set_property_async);
    VP_MPV_FN(get_property_async);
    VP_MPV_FN(command_async);
    VP_MPV_FN(wait_event);
    VP_MPV_FN(set_wakeup_callback);
    VP_MPV_FN(observe_property);
    VP_MPV_FN(get_property);
    VP_MPV_FN(get_property_string);
    VP_MPV_FN(set_property_string);
    VP_MPV_FN(request_log_messages);
    VP_MPV_FN(free);
    VP_MPV_FN(free_node_contents);
    VP_MPV_FN(error_string);
    VP_MPV_FN(hook_add);      // optional: without it enhancement falls back to FILE_LOADED
    VP_MPV_FN(hook_continue);
#undef VP_MPV_FN
};

MpvBackend::MpvBackend() = default;
MpvBackend::~MpvBackend() { Shutdown(); }

bool MpvBackend::LoadApi(const std::filesystem::path& dll, std::string* error) {
    library_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library_) {
        error_code_ = "LIBMPV_MISSING";
        if (error) *error = "libmpv-2.dll is missing or could not be loaded beside VideoPlayer.exe. Rebuild or restore the MPV runtime files.";
        return false;
    }
    api_ = std::make_unique<Api>();
#define VP_LOAD_MPV(name) \
    api_->name = reinterpret_cast<decltype(api_->name)>(GetProcAddress(library_, "mpv_" #name)); \
    if (!api_->name) { error_code_ = "LIBMPV_MISSING"; if (error) *error = "libmpv-2.dll is missing required entry point mpv_" #name; return false; }
    VP_LOAD_MPV(create)
    VP_LOAD_MPV(initialize)
    VP_LOAD_MPV(terminate_destroy)
    VP_LOAD_MPV(set_option_string)
    VP_LOAD_MPV(set_property)
    VP_LOAD_MPV(set_property_async)
    VP_LOAD_MPV(get_property_async)
    VP_LOAD_MPV(command_async)
    VP_LOAD_MPV(wait_event)
    VP_LOAD_MPV(set_wakeup_callback)
    VP_LOAD_MPV(observe_property)
    VP_LOAD_MPV(get_property)
    VP_LOAD_MPV(get_property_string)
    VP_LOAD_MPV(set_property_string)
    VP_LOAD_MPV(request_log_messages)
    VP_LOAD_MPV(free)
    VP_LOAD_MPV(free_node_contents)
    VP_LOAD_MPV(error_string)
#undef VP_LOAD_MPV
    api_->hook_add = reinterpret_cast<decltype(api_->hook_add)>(GetProcAddress(library_, "mpv_hook_add"));
    api_->hook_continue = reinterpret_cast<decltype(api_->hook_continue)>(GetProcAddress(library_, "mpv_hook_continue"));
    return true;
}

bool MpvBackend::SetOption(const char* name, const std::string& value, bool required, std::string* error) {
    const int result = api_->set_option_string(ctx_, name, value.c_str());
    if (result >= 0) return true;
    const std::string detail = MpvErrorText(api_->error_string, result);
    AppendLog(std::string("option rejected: ") + name + "=" + value + " (" + detail + ")");
    if (required && error) *error = std::string("libmpv rejected required option ") + name + ": " + detail;
    return !required;
}

bool MpvBackend::Initialize(HWND host, HWND video_child, const std::filesystem::path& exe_dir,
                            const std::filesystem::path& data_dir, const std::string& config_json,
                            WakeFn wake, EventFn event, std::string* error) {
    if (initialize_attempted_) {
        if (error) *error = init_failure_.empty()
            ? "libmpv initialization is process-scoped and has already been attempted. Restart the player to retry."
            : init_failure_;
        return false;
    }
    // --- core-reliability 1A (GPU adapter preflight) begin: validate the saved GPU against the current adapters BEFORE the
    // process-scoped gate, the libmpv load and mpv_create (report 25-A §3). A blocked preflight returns with
    // initialize_attempted_ false and the gate unclaimed, so a re-selection initialises in this process.
    data_dir_ = data_dir;
    log_path_ = data_dir / L"logs" / L"mpv.log";
    host_log_path_ = data_dir / L"logs" / L"mpv-host.log";
    {
        std::error_code log_dir_ec;
        std::filesystem::create_directories(host_log_path_.parent_path(), log_dir_ec);
    }
    StartLogWriter(); // idempotent; a failure is reported after the gate, as before
    const Json config = Json::Parse(config_json);
    const auto preflight_started = std::chrono::steady_clock::now();
    adapters_ = gpu::EnumerateAdapters();
    saved_adapter_ = gpu::ParseSavedSelection(config);
    adapter_resolution_ = gpu::Reconcile(saved_adapter_, adapters_, config["video"]["gpuApi"].str("d3d11") == "d3d11");
    adapter_preflight_done_ = true;
    adapter_preflight_blocked_ = gpu::IsBlocking(adapter_resolution_.outcome);
    const std::string preflight_line = gpu::PreflightLogLine(saved_adapter_, adapters_, adapter_resolution_);
    const bool preflight_changed = preflight_line != last_preflight_line_;
    if (preflight_changed) {
        last_preflight_line_ = preflight_line;
        AppendLog(preflight_line + " ms=" + Number(MsSince(preflight_started), 1));
    }
    if (adapter_preflight_blocked_) {
        error_code_ = gpu::ErrorCodeFor(adapter_resolution_.outcome);
        init_failure_ = gpu::UserMessage(saved_adapter_, adapters_, adapter_resolution_);
        if (preflight_changed)
            AppendLog("gpu adapter preflight blocked: libmpv not loaded, process gate not claimed; waiting for re-selection (code=" +
                      error_code_ + ")");
        if (error) *error = init_failure_;
        return false;
    }
    adapter_persist_pending_ = adapter_resolution_.outcome == gpu::Outcome::Reconciled ||
                               adapter_resolution_.outcome == gpu::Outcome::ExactUnverified;
    // --- core-reliability 1A end ---
    bool expected = false;
    if (!g_mpv_process_initialization_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        initialize_attempted_ = true;
        init_failure_ = "A libmpv context has already been attempted in this process. Restart the player to retry.";
        if (error) *error = init_failure_;
        return false;
    }
    initialize_attempted_ = true;
    initialized_ = false;
    init_failure_.clear();
    const auto init_started = std::chrono::steady_clock::now();
    error_code_ = "MPV_INIT";
    last_state_.clear();
    last_position_emit_ = {};
    ended_emitted_ = false;
    active_adapter_.clear();
    host_ = host;
    video_child_ = video_child;
    exe_dir_ = exe_dir;
    data_dir_ = data_dir;
    enhancement_root_ = exe_dir / L"mpv";
#if defined(VP_TEST_HARNESS)
    wchar_t test_asset_root[32768]{};
    const DWORD test_asset_length = GetEnvironmentVariableW(L"VP_TEST_MPV_ASSET_DIR", test_asset_root,
                                                             static_cast<DWORD>(std::size(test_asset_root)));
    if (test_asset_length > 0 && test_asset_length < std::size(test_asset_root))
        enhancement_root_ = std::filesystem::path(test_asset_root);
#endif
    wake_ = std::move(wake);
    event_ = std::move(event);
    log_path_ = data_dir / L"logs" / L"mpv.log";
    host_log_path_ = data_dir / L"logs" / L"mpv-host.log";
    std::error_code ec;
    std::filesystem::create_directories(log_path_.parent_path(), ec);
    if (!StartLogWriter()) {
        init_failure_ = "Could not start the buffered MPV host log writer.";
        if (error) *error = init_failure_;
        return false;
    }

    // Add only the app-local runtime directories. In particular this keeps the
    // optional VapourSynth subset ahead of any machine-wide Python/VapourSynth.
    for (const auto& dir : {exe_dir, exe_dir / L"vs-runtime",
                            exe_dir / L"vs-runtime" / L"vs-plugins" / L"vsort"}) {
        if (std::filesystem::exists(dir, ec)) {
            DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(dir.c_str());
            if (cookie) dll_directories_.push_back(reinterpret_cast<void*>(cookie));
        }
    }
    if (!LoadApi(exe_dir / L"libmpv-2.dll", error)) {
        init_failure_ = error ? *error : "Could not load libmpv.";
        return false;
    }
    ctx_ = api_->create();
    if (!ctx_) {
        init_failure_ = "libmpv could not create a playback context.";
        if (error) *error = init_failure_;
        return false;
    }

    const Json video = config["video"];
    video_config_ = video;
    const Json audio = config["audio"];
    const std::string gpu_api = video["gpuApi"].str("d3d11");
    const std::string configured_hwdec = video["hwdec"].str("d3d11va");
    const std::string hwdec = ResolveHwdec(gpu_api, configured_hwdec);
    gpu_api_ = gpu_api;
    base_hwdec_ = hwdec;
    current_audio_device_ = audio["device"].str();
    audio_exclusive_ = audio["exclusive"].boolean(false) ? "yes" : "no";
    audio_channels_ = audio["channels"].str("stereo");
    // Configs written before the volume control have no audio.volume / audio.mute: 100 % and unmuted, as before.
    audio_volume_ = static_cast<int>(std::clamp<std::int64_t>(audio["volume"].i64(100), 0, 100));
    audio_mute_ = audio["mute"].boolean(false);
    // audio.masterClock: absent in every config written before the Master Clock setting. Absent -> auto (the new
    // default, mpv-lazy's vsync_auto). Logged so the one behaviour change is visible: earlier builds forced the audio
    // clock for VS / AMF interpolation even with bit-perfect off; only 音訊優先 does that now (bit-perfect on is
    // unaffected: always the audio clock).
    const std::string stored_clock = audio["masterClock"].str();
    master_clock_ = NormalizeMasterClock(stored_clock);
    const std::string clock_source = stored_clock.empty() ? "default" : stored_clock == master_clock_ ? "stored" : "invalid";
    if (clock_source != "stored")
        AppendLog("master-clock " + clock_source + (stored_clock.empty() ? std::string() : " value=" + stored_clock) + " -> " + master_clock_ +
                  " (no audio.masterClock saved: Automatic = mpv-lazy vsync_auto; the old forced audio clock for VS/AMF"
                  " interpolation now only applies with 音訊優先 or bit-perfect)");
    SetSeekPreference(config["playback"]["seekMode"].str("exact"));
    // Core-reliability 1A: the adapter comes from the preflight snapshot (same semantics as the former FindAdapterName /
    // CudaIndexForHardwareAdapter: hardware index skips software adapters; Auto = the first hardware adapter).
    const bool explicit_adapter = adapter_resolution_.outcome != gpu::Outcome::Auto;
    const gpu::AdapterInfo* const chosen_adapter =
        adapter_resolution_.adapter >= 0 && adapter_resolution_.adapter < static_cast<int>(adapters_.size()) ? &adapters_[adapter_resolution_.adapter] : nullptr;
    active_adapter_ = chosen_adapter ? chosen_adapter->name : std::string();
    const std::uint32_t vendor_id = chosen_adapter ? chosen_adapter->vendor : 0;
    gpu_device_id_ = chosen_adapter ? chosen_adapter->hardware_index : 0;
    gpu_vendor_ = GpuVendorName(vendor_id);
    cuda_device_id_ = gpu_vendor_ == "nvidia" ? gpu::CudaOrdinal(adapters_, gpu_device_id_) : 0;
    // Not mapped to vulkan-device: mpv rejects an unknown device name at option validation instead of falling back
    // (CLI test: "No device with name '...'!" and no VO), and DXGI descriptions are not guaranteed Vulkan names.
    if (explicit_adapter && gpu_api != "d3d11") AppendLog("configured D3D11 adapter ignored because gpu-api is " + gpu_api);

    const auto handle = reinterpret_cast<std::uintptr_t>(video_child_);
    std::vector<std::pair<const char*, std::string>> required = {
        {"wid", std::to_string(handle)}, {"vo", "gpu-next"}, {"gpu-api", gpu_api}, {"hwdec", hwdec}, {"ao", "wasapi"},
    };
    if (explicit_adapter && gpu_api == "d3d11" && !active_adapter_.empty())
        required.emplace_back("d3d11-adapter", active_adapter_);
    for (const auto& [name, value] : required)
        if (!SetOption(name, value, true, error)) {
            init_failure_ = error ? *error : std::string("libmpv rejected required option ") + name;
            if (std::string_view(name) == "d3d11-adapter") { // 1A: vanished after the preflight; libmpv is already process-scoped
                error_code_ = "GPU_ADAPTER_NOT_FOUND";
                init_failure_ = "所選的 GPU（" + value + "）在啟動時無法使用。請重新啟動播放器，再到「設定 → 影像」重新選擇 GPU 或改用自動。";
                if (error) *error = init_failure_;
            }
            return false;
        }

    // Optional config key video.subtitleFont; an empty value keeps the default instead of libass's sans-serif (Arial).
    std::string sub_font = video["subtitleFont"].str();
    if (sub_font.empty()) sub_font = "Microsoft JhengHei";
    const std::vector<std::pair<const char*, std::string>> options = {
        {"config", "no"}, {"terminal", "no"}, {"idle", "yes"}, {"osc", "no"}, {"osd-level", "0"},
        {"input-default-bindings", "no"}, {"input-builtin-bindings", "no"}, {"input-vo-keyboard", "no"},
        {"input-cursor", "no"}, {"cursor-autohide", "no"}, {"input-media-keys", "no"}, {"ytdl", "no"},
        {"load-scripts", "no"}, {"load-stats-overlay", "no"}, {"load-console", "no"}, {"load-select", "no"},
        {"load-positioning", "no"}, {"load-commands", "no"}, {"load-auto-profiles", "no"}, {"load-context-menu", "no"},
        {"force-window", "yes"}, {"d3d11-flip", "yes"},
        {"save-position-on-quit", "no"}, {"resume-playback", "no"}, {"sub-auto", "fuzzy"},
        {"sub-codepage", "auto"}, {"embeddedfonts", "yes"}, {"sub-ass-force-margins", "yes"},
        // libass default family = fallback for ASS styles whose font is not installed, and the SRT font.
        // Must be a CJK font with Latin glyphs and a real Bold. NOT "Noto Sans TC": Windows ships it as a
        // variable font that libass resolves to Thin. Without this, missing fonts fall back to Arial + JhengHei UI.
        {"sub-font", sub_font},
        {"sub-fonts-dir", Utf8((data_dir / L"fonts").wstring())},
        {"subs-fallback", "yes"}, {"slang", "cht,zh-tw,zh-hant,chi,zho,zh,chs,sc,zh-cn"},
        {"hr-seek", "yes"},
        // Core-reliability 3A: mpv-lazy mpv.conf:125. mpv inserts its own deinterlacer only for frames the decoder flags
        // as interlaced (d3d11vpp on native d3d11va frames, bwdif send_field on copy-back / software frames, i.e. always
        // under a VapourSynth stage); progressive sources get none. Output is the field rate, so a VapourSynth stage then
        // receives 2x container-fps (see ApplyEnhancement's 7.2a gate and RunPendingRateReapply).
        {"deinterlace", "auto"},
        {"hr-seek-framedrop", "no"}, {"audio-exclusive", audio_exclusive_}, {"demuxer-cache-dir", Utf8((data_dir / L"cache" / L"mpv").wstring())},
        {"gpu-shader-cache-dir", Utf8((data_dir / L"cache" / L"mpv-shader").wstring())},
        {"icc-cache-dir", Utf8((data_dir / L"cache" / L"mpv-icc").wstring())},
        {"demuxer-max-bytes", "1024MiB"}, {"demuxer-max-back-bytes", "512MiB"},
        {"audio-channels", audio["channels"].str("stereo")},
        // Bit-perfect coupling (ResolveAudioSyncPolicy): the output level is mpv's software volume (cubic); 100 is
        // bit-identical. volume-gain / replaygain / af stay at mpv's neutral defaults and are never changed by this
        // host; video-sync / interpolation start at the bit-perfect-safe values until the first file decides.
        {"volume", std::to_string(audio_exclusive_ == "yes" ? 100 : audio_volume_)}, {"mute", audio_mute_ ? "yes" : "no"},
        {"volume-gain", "0"}, {"replaygain", "no"},
        {"audio-pitch-correction", audio_exclusive_ == "yes" ? "no" : "yes"},
        {"video-sync", "audio"}, {"interpolation", "no"},
        {"log-file", Utf8(log_path_.wstring())}, {"keep-open", "yes"},
    };
    for (const auto& folder : {data_dir / L"cache" / L"mpv", data_dir / L"cache" / L"mpv-shader", data_dir / L"cache" / L"mpv-icc",
                               data_dir / L"fonts"})
        std::filesystem::create_directories(folder, ec);
    for (const auto& [name, value] : options) SetOption(name, value, false, nullptr);
    if (!current_audio_device_.empty()) SetOption("audio-device", current_audio_device_, false, nullptr);

    const int result = api_->initialize(ctx_);
    if (result < 0) {
        init_failure_ = "libmpv initialization failed: " + MpvErrorText(api_->error_string, result);
        if (error) *error = init_failure_;
        return false;
    }
    initialized_ = true;
    // Version strings for the diagnostics export: read once here, while no file (and no VapourSynth chain) exists.
    for (const auto& [property, target] : {std::pair<const char*, std::string*>{"mpv-version", &mpv_version_},
                                           std::pair<const char*, std::string*>{"ffmpeg-version", &ffmpeg_version_}}) {
        if (char* value = api_->get_property_string(ctx_, property)) {
            *target = value;
            api_->free(value);
        }
    }
    // Core-reliability 1B: the retry timer exists before the callback is registered (registration calls it once).
    wake_stopping_.store(false, std::memory_order_release);
#if defined(VP_FAULT_INJECTION)
    AppendLog(LoadFaultConfig());
#endif
    wake_retry_timer_ = CreateThreadpoolTimer(&MpvBackend::OnWakeRetry, this, nullptr);
    if (!wake_retry_timer_ && host_) {
        // Degraded mode (practically never): a 1 s UI timer that drains only after a failed wake.
        AppendLog("mpv wake retry timer unavailable (error=" + std::to_string(GetLastError()) + "); 1 s watchdog drains after a failed wake");
        wake_watchdog_ = SetTimer(host_, kWakeWatchdogTimerId, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
            if (g_timer_backend && g_timer_backend->wake_post_failures_.load(std::memory_order_acquire) > 0 && g_timer_backend->Ready())
                g_timer_backend->PollEvents();
        }) != 0;
    }
    api_->set_wakeup_callback(ctx_, &MpvBackend::OnMpvWakeup, this);
    api_->request_log_messages(ctx_, "info");
    ObserveProperties();
    g_timer_backend = this;
    // on_preloaded runs after the demuxer opened the file and before decoders and filters are
    // created: the enhancement chain set there is built once, instead of being torn down and
    // rebuilt right after FILE_LOADED (which delayed the first frame by seconds with DirectML).
    hooks_available_ = api_->hook_add && api_->hook_continue &&
        api_->hook_add(ctx_, kPreloadHookId, "on_preloaded", 0) >= 0;
    if (!hooks_available_) AppendLog("on_preloaded hook unavailable; enhancement is applied after FILE_LOADED");
    std::string catalog_error;
    enhancement_configured_ = enhancement_compiler_.Load(enhancement_root_ / L"catalog.json", &catalog_error);
    if (!enhancement_configured_) AppendLog(catalog_error);
    trt_installed_ = TensorRtRuntimeInstalled(exe_dir_);
    AppendLog("libmpv initialized; GPU API=" + gpu_api + "; hwdec=" + hwdec + " (configured " + configured_hwdec +
              "); audio=wasapi; adapter=" + (active_adapter_.empty() ? std::string("auto") : active_adapter_) +
              "; vendor=" + gpu_vendor_ + "; dml_device=" + std::to_string(gpu_device_id_) +
              (gpu_vendor_ == "nvidia" ? "; cuda_device=" + std::to_string(cuda_device_id_) : std::string()) +
              "; tensorrt=" + (trt_installed_ ? "installed" : "absent") + "; init_ms=" + Number(MsSince(init_started), 1) +
              "; luid=" + (chosen_adapter ? std::to_string(chosen_adapter->luid) : std::string("-")) +
              "; adapter_outcome=" + gpu::OutcomeName(adapter_resolution_.outcome));
    AppendLog("audio init bitperfect=" + std::string(audio_exclusive_ == "yes" ? "yes" : "no") + " exclusive=" + audio_exclusive_ +
              " volume=" + std::to_string(audio_exclusive_ == "yes" ? 100 : audio_volume_) + " saved_volume=" +
              std::to_string(audio_volume_) + " mute=" + (audio_mute_ ? "yes" : "no") + " channels=" + audio_channels_ +
              " master_clock=" + master_clock_ + " effective_clock=" + (audio_exclusive_ == "yes" ? "audio(bitperfect)" : master_clock_ == "audio" ? "audio" : "per-file(vsync_auto)") +
              " seek_pref=" + seek_pref_);
    return true;
}

std::string MpvBackend::AdapterStatusJson() const {
    return adapter_preflight_done_ ? gpu::StatusJson(saved_adapter_, adapters_, adapter_resolution_, initialize_attempted_) : std::string();
}

bool MpvBackend::TakeAdapterPersist(AdapterPersist* persist) {
    if (!adapter_persist_pending_ || !persist || adapter_resolution_.adapter < 0 ||
        adapter_resolution_.adapter >= static_cast<int>(adapters_.size()))
        return false;
    adapter_persist_pending_ = false;
    persist->adapter = adapters_[adapter_resolution_.adapter];
    persist->outcome = gpu::OutcomeName(adapter_resolution_.outcome);
    persist->old_luid = saved_adapter_.luid_text;
    persist->source = saved_adapter_.source;
    return true;
}

void MpvBackend::ObserveProperties() {
    if (!ctx_ || !initialized_) return;
    const struct { const char* name; mpv_format format; std::uint64_t id; } properties[] = {
        {"time-pos", MPV_FORMAT_DOUBLE, 1}, {"duration", MPV_FORMAT_DOUBLE, 2}, {"pause", MPV_FORMAT_FLAG, 3},
        {"core-idle", MPV_FORMAT_FLAG, 4}, {"seeking", MPV_FORMAT_FLAG, 5},
        {"paused-for-cache", MPV_FORMAT_FLAG, 6}, {"eof-reached", MPV_FORMAT_FLAG, 7}, {"idle-active", MPV_FORMAT_FLAG, 8},
        {"track-list", MPV_FORMAT_NODE, 9}, {"chapter-list", MPV_FORMAT_NODE, 10},
        {"video-params", MPV_FORMAT_NODE, 11}, {"video-out-params", MPV_FORMAT_NODE, 12},
        {"audio-params", MPV_FORMAT_NODE, 13}, {"audio-out-params", MPV_FORMAT_NODE, 14},
        {"container-fps", MPV_FORMAT_DOUBLE, 15}, {"estimated-vf-fps", MPV_FORMAT_DOUBLE, 16},
        {"display-fps", MPV_FORMAT_DOUBLE, 17}, {"frame-drop-count", MPV_FORMAT_INT64, 18},
        {"decoder-frame-drop-count", MPV_FORMAT_INT64, 19}, {"avsync", MPV_FORMAT_DOUBLE, 20},
        {"hwdec-current", MPV_FORMAT_STRING, 21},
        {"video-codec-name", MPV_FORMAT_STRING, 23}, {"audio-codec-name", MPV_FORMAT_STRING, 24},
        {"sid", MPV_FORMAT_STRING, 25}, {"aid", MPV_FORMAT_STRING, 26},
        {"glsl-shaders", MPV_FORMAT_NODE, 27}, {"vf", MPV_FORMAT_NODE, 28},
        {"speed", MPV_FORMAT_DOUBLE, 29}, // cached only: the frameProcessing rate window (no handler beyond the reset)
        {"display-names", MPV_FORMAT_STRING, 30}, // cached only: frameProcessing render.display (no handler)
        {"deinterlace-active", MPV_FORMAT_FLAG, 31}, // 3A: mpv's auto deinterlacer is running (VS input = field rate)
    };
    for (const auto& property : properties) {
        const int result = api_->observe_property(ctx_, property.id, property.name, property.format);
        if (result < 0) AppendLog(std::string("property observation unavailable: ") + property.name + " (" + MpvErrorText(api_->error_string, result) + ")");
    }
}

void MpvBackend::OnMpvWakeup(void* opaque) {
    auto* self = static_cast<MpvBackend*>(opaque);
    if (!self || !self->wake_ || self->wake_stopping_.load(std::memory_order_acquire)) return;
#if defined(VP_FAULT_INJECTION)
    ++g_wake_callbacks;
#endif
    if (self->wake_pending_.exchange(true, std::memory_order_acq_rel)) return; // a wake token is already outstanding
    self->DeliverWake();
}

void CALLBACK MpvBackend::OnWakeRetry(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
    auto* self = static_cast<MpvBackend*>(context);
    if (!self || self->wake_stopping_.load(std::memory_order_acquire)) return;
#if defined(VP_FAULT_INJECTION)
    ++g_wake_retry_fires;
#endif
    self->wake_retries_.fetch_add(1, std::memory_order_relaxed);
    self->DeliverWake(); // the token (wake_pending_) is still held: this is the outstanding wake
}

// mpv's wakeup callback or the retry timer, never the UI thread; atomics and SetThreadpoolTimer only.
void MpvBackend::DeliverWake() {
    unsigned long error = 0;
    bool injected = false;
#if defined(VP_FAULT_INJECTION)
    injected = InjectWakeFailure(&error);
#endif
    if (!injected) error = wake_();
    if (error == 0) {
#if defined(VP_FAULT_INJECTION)
        ++g_wake_posts_ok;
#endif
        wake_retry_ms_.store(0, std::memory_order_relaxed); // token = the queued WM_APP_MPV_WAKE
        return;
    }
#if defined(VP_FAULT_INJECTION)
    ++g_wake_posts_failed;
#endif
    wake_last_error_.store(static_cast<std::uint32_t>(error), std::memory_order_relaxed);
    if (wake_post_failures_.fetch_add(1, std::memory_order_acq_rel) == 0)
        wake_failed_since_ns_.store(SteadyNowNs(), std::memory_order_relaxed);
#if defined(VP_FAULT_INJECTION)
    if (g_fault.wake_legacy) return; // HEAD 9202426 semantics: the token stays set and nothing retries (T0)
#endif
    if (error == ERROR_INVALID_WINDOW_HANDLE || wake_stopping_.load(std::memory_order_acquire) || !wake_retry_timer_) {
        // Window gone (shutdown), or degraded mode (the 1 s watchdog drains): release the token, no retry.
        wake_pending_.store(false, std::memory_order_release);
        return;
    }
    // Token = the armed retry (wake_pending_ stays true). Backoff 16, 32, ... 1000 ms; no timer while healthy.
    const std::uint32_t previous = wake_retry_ms_.load(std::memory_order_relaxed);
    const std::uint32_t ms = previous == 0 ? 16u : std::min<std::uint32_t>(previous * 2u, 1000u);
    wake_retry_ms_.store(ms, std::memory_order_relaxed);
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(ms) * 10000); // negative = relative, 100 ns units
    FILETIME due_time{due.LowPart, due.HighPart};
    SetThreadpoolTimer(wake_retry_timer_, &due_time, 0, 0);
}

// BeginShutdown (UI thread), after the wakeup callback was removed: no retry can be armed or run after this.
void MpvBackend::CloseWakeRetry() {
#if defined(VP_FAULT_INJECTION)
    AppendLog("wake-stats at shutdown callbacks=" + std::to_string(g_wake_callbacks.load()) + " posts_ok=" + std::to_string(g_wake_posts_ok.load()) +
              " posts_failed=" + std::to_string(g_wake_posts_failed.load()) + " retries=" + std::to_string(g_wake_retry_fires.load()) +
              " drains=" + std::to_string(g_drains.load()) + " retry_ms=" + std::to_string(wake_retry_ms_.load()) +
              " token=" + (wake_pending_.load() ? "held" : "free") + " timer=" + (wake_retry_timer_ ? "open" : "none"));
#endif
    if (wake_retry_timer_) {
        // Twice: a retry already inside DeliverWake when stopping was set may re-arm once; the second pass cancels it.
        for (int pass = 0; pass < 2; ++pass) {
            SetThreadpoolTimer(wake_retry_timer_, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(wake_retry_timer_, TRUE);
        }
        CloseThreadpoolTimer(wake_retry_timer_);
        wake_retry_timer_ = nullptr;
    }
    if (wake_watchdog_ && host_) KillTimer(host_, kWakeWatchdogTimerId);
    wake_watchdog_ = false;
}

void MpvBackend::PollEvents() {
    wake_pending_.store(false, std::memory_order_release);
    if (!ctx_ || !api_ || !initialized_) return;
    // Report 25-B F4: never a nested drain (an mpv_event pointer is only valid until the next wait_event). After the
    // clear above, so a nested call cannot leave the token set while the outer loop drains to MPV_EVENT_NONE.
    if (polling_) {
#if defined(VP_FAULT_INJECTION)
        ++g_reentrant_drains;
#endif
        return;
    }
    // Core-reliability 1B: a wake post failed since the last drain and was retried (or the watchdog drained).
    if (const std::uint32_t failures = wake_post_failures_.exchange(0, std::memory_order_acq_rel)) {
        const std::int64_t since = wake_failed_since_ns_.exchange(0, std::memory_order_relaxed);
        const std::uint32_t retries = wake_retries_.exchange(0, std::memory_order_relaxed);
        AppendLog("mpv wake post failed n=" + std::to_string(failures) + " last_error=" +
                  std::to_string(wake_last_error_.load(std::memory_order_relaxed)) + " retries=" + std::to_string(retries) +
                  " recovered_after_ms=" + (since > 0 ? Number((SteadyNowNs() - since) / 1e6, 1) : std::string("?")));
    }
    // UI-thread stall accounting: the drain (every handler, including synchronous libmpv reads) and the post-busy work
    // after it are timed; anything >= 50 ms is logged with the slowest event, so a log shows the longest UI stall.
    const auto drain_started = std::chrono::steady_clock::now();
    int events = 0;
    int slowest_id = 0;
    double slowest_ms = 0.0;
    polling_ = true;
    for (;;) {
        mpv_event* event = api_->wait_event(ctx_, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;
        const int id = static_cast<int>(event->event_id);
        const auto event_started = std::chrono::steady_clock::now();
#if defined(VP_FAULT_INJECTION)
        const bool fp_reply = event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata >= kFpReplyBase &&
                              event->reply_userdata - kFpReplyBase < kFpReplyRange;
#endif
        OnEvent(event);
        ++events;
        const double ms = MsSince(event_started);
#if defined(VP_FAULT_INJECTION)
        if (fp_reply) {
            g_fp_tick_acc += ms;
            if (fp_phase_ == 0) { g_fp_tick_ms.push_back(g_fp_tick_acc); g_fp_tick_acc = 0.0; }
        }
#endif
        if (ms > slowest_ms) { slowest_ms = ms; slowest_id = id; }
    }
    polling_ = false;
    const double drain_ms = MsSince(drain_started);
#if defined(VP_FAULT_INJECTION)
    ++g_drains;
    if (events == 0) ++g_empty_drains;
    g_drain_ms.push_back(drain_ms);
    if (std::chrono::steady_clock::now() - g_wake_stats_at >= std::chrono::seconds(10)) {
        g_wake_stats_at = std::chrono::steady_clock::now();
        std::vector<double> d = std::move(g_drain_ms);
        g_drain_ms.clear();
        std::sort(d.begin(), d.end());
        const auto pick = [&](double p) { return d.empty() ? 0.0 : d[std::min(d.size() - 1, static_cast<std::size_t>(d.size() * p))]; };
        std::vector<double> t = std::move(g_fp_tick_ms);
        g_fp_tick_ms.clear();
        std::sort(t.begin(), t.end());
        AppendLog("fp-tick-stats n=" + std::to_string(t.size()) + " p50_ms=" + Number(t.empty() ? 0.0 : t[t.size() / 2], 3) +
                  " max_ms=" + Number(t.empty() ? 0.0 : t.back(), 3) + " (request + snapshot reply handling + build/emit, UI thread)");
        AppendLog("drain-stats n=" + std::to_string(d.size()) + " p50_ms=" + Number(pick(0.5), 3) + " p99_ms=" + Number(pick(0.99), 3) +
                  " max_ms=" + Number(d.empty() ? 0.0 : d.back(), 3) + " over_1.15ms=" +
                  std::to_string(std::count_if(d.begin(), d.end(), [](double v) { return v > 1.15; })));
        AppendLog("wake-stats callbacks=" + std::to_string(g_wake_callbacks.load()) + " posts_ok=" + std::to_string(g_wake_posts_ok.load()) +
                  " posts_failed=" + std::to_string(g_wake_posts_failed.load()) + " injected=" + std::to_string(g_wake_injected.load()) +
                  " retries=" + std::to_string(g_wake_retry_fires.load()) + " drains=" + std::to_string(g_drains.load()) +
                  " empty_drains=" + std::to_string(g_empty_drains.load()) + " reentrant=" + std::to_string(g_reentrant_drains.load()));
    }
#endif
    // An end condition met inside the drain (PLAYBACK_RESTART, a probe reply) is completed only now: an MPV_EVENT_SEEK
    // that was queued behind it has re-joined the busy period above, and the post-busy synchronous reads must not
    // run while that seek's script reload holds mpv's core (audit 7.3a).
    double post_ms = -1.0;
    if (vs_busy_ && !vs_end_pending_by_.empty()) {
        const std::string by = std::move(vs_end_pending_by_);
        vs_end_pending_by_.clear();
        const auto post_started = std::chrono::steady_clock::now();
        FinishVsBusy(by.c_str());
        post_ms = MsSince(post_started);
    }
    // Audit 7.2c: a VapourSynth failure seen in this drain (log handler) only marked the cleanup; do it here.
    RunPendingDeadVsCleanup("event-loop");
    RunPendingRateReapply("event-loop"); // 3A: same point and guards as the 7.2c cleanup
    if (drain_ms >= 50.0 || post_ms >= 50.0)
        AppendLog("ui-thread stall drain_ms=" + Number(drain_ms, 1) + " events=" + std::to_string(events) +
                  " slowest_event=" + std::to_string(slowest_id) + " slowest_ms=" + Number(slowest_ms, 1) +
                  (post_ms >= 0.0 ? " post_busy_ms=" + Number(post_ms, 1) : std::string()));
}

void MpvBackend::OnEvent(::mpv_event* event) {
    if (!event) return;
    switch (event->event_id) {
    case MPV_EVENT_SET_PROPERTY_REPLY:
        OnAsyncReply(event->reply_userdata, event->error, "set");
        break;
    case MPV_EVENT_COMMAND_REPLY:
        // 2B: a loadfile reply is matched to its load request first (stale ones are ignored), then logged as before.
        if (load_replies_.count(event->reply_userdata)) OnLoadfileReply(event->reply_userdata, event->error, event->data);
        OnAsyncReply(event->reply_userdata, event->error, "command");
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY: {
        // Busy probes (OnVsBusyTick, the barrier, the reconfig watch): a reply means mpv's core served the request,
        // i.e. the playback thread is no longer inside the VapourSynth script load that started at the vf change.
        // The frameProcessing snapshot's replies (report 23) have their own id range.
        const std::uint64_t id = event->reply_userdata;
        if (id >= kFpReplyBase && id - kFpReplyBase < kFpReplyRange) {
            OnFrameProcessingReply(id, event->error, event->data);
            break;
        }
        pending_replies_.erase(id);
        if (id != 0 && id == vs_probe_id_) {
            const int kind = vs_probe_kind_;
            vs_probe_id_ = 0;
            vs_probe_kind_ = 0;
            const auto* property = static_cast<mpv_event_property*>(event->data);
            const double fps = event->error >= 0 && property && property->format == MPV_FORMAT_DOUBLE && property->data
                ? *static_cast<double*>(property->data) : 0.0;
            if (!vs_busy_) break;
            if (kind == 1) {
                if (fps > 0.0) RequestVsBusyEnd("probe");
            } else if (kind == 4) {
                // Reconfig watch (7.3b): mpv served it only once its core was free. A long wait means the request sat
                // behind the script reload, which has now finished; a short one says nothing during the drain phase.
                const double waited = MsSince(vs_probe_sent_);
                ++reconfig_watch_probes_;
                reconfig_watch_max_ms_ = std::max(reconfig_watch_max_ms_, waited);
                if (waited >= kReconfigReloadObservedMs) {
                    AppendLog("vs-init reconfig reload observed (watch probe waited " + Number(waited, 0) + " ms, probe " +
                              std::to_string(reconfig_watch_probes_) + ")");
                    RequestVsBusyEnd("reload-observed", true);
                } else if (std::chrono::steady_clock::now() - vs_busy_since_ >= kReconfigFallback) {
                    AppendLog("vs-init reconfig no reload observed within " + std::to_string(kReconfigFallback.count()) +
                              " s (watch probes=" + std::to_string(reconfig_watch_probes_) + " longest_wait_ms=" +
                              Number(reconfig_watch_max_ms_, 0) + ")");
                    RequestVsBusyEnd("reconfig-timeout", true);
                }
            } else if (kind == 2) {
                SendVsBusyProbe(3); // barrier step 2: served after the iteration that served step 1 has run its queued seek
            } else if (kind == 3) {
                AppendLog("vs-init barrier passed by=" + vs_barrier_by_ + " barrier_ms=" + Number(MsSince(vs_barrier_since_), 0) +
                          " seeks=" + std::to_string(vs_seek_events_) + "/" + std::to_string(vs_seek_cmds_));
                const std::string by = vs_barrier_by_ + "+barrier";
                vs_barrier_by_.clear();
                // The barrier is the strongest end condition there is: no further check, finish (after the drain).
                if (polling_) vs_end_pending_by_ = by;
                else FinishVsBusy(by.c_str());
            }
        }
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        const auto* message = static_cast<mpv_event_log_message*>(event->data);
        if (message && message->text) {
            std::string text(message->text);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            HandleLogLine(message->prefix ? message->prefix : "", message->level ? message->level : "", text);
        }
        break;
    }
    case MPV_EVENT_START_FILE:
        ended_emitted_ = false;
        display_sync_used_ = false; // a new file gets a new audio chain
        opened_hwdec_.clear();
        last_nonfatal_error_.clear();
        ResetPerFileDiagnostics();
        AppendLog(std::string("open started") + (pending_open_ ? " surface=kept" : ""));
        // An Open() keeps whatever the child shows (the previous episode's last image, or the
        // child is already hidden; RecoverDeviceLoss hides it itself); a load nobody requested hides it.
        if (!pending_open_ && video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_HIDE);
        {   // 2B: the entry the current load request's reply named (or the first start after a reply without an id)
            const auto* started = static_cast<const mpv_event_start_file*>(event->data);
            const std::int64_t entry = started ? started->playlist_entry_id : 0;
            if (load_.gen && load_.reply_seen && (entry == load_.entry_id || load_.entry_id == -1)) {
                load_.entry_id = entry;
                load_.started = true;
            } else {
                AppendLog("start-file entry=" + std::to_string(entry) + " (not the current load request" +
                          (load_.gen ? " gen=" + std::to_string(load_.gen) + " entry=" + std::to_string(load_.entry_id) : std::string(": none")) + ")");
            }
        }
        EmitState("opening");
        break;
    case MPV_EVENT_HOOK: {
        const auto* hook = static_cast<mpv_event_hook*>(event->data);
        if (hook && event->reply_userdata == kPreloadHookId) ApplyFileEnhancement(true);
        // Synchronous by API (it takes mpv's core lock); safe: mpv's core is parked in the hook waiting for this call.
        if (hook && api_ && api_->hook_continue) api_->hook_continue(ctx_, hook->id);
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        // 2B: the request is complete once its entry loaded (also when its reply came but the start was not matched, so
        // a request can never outlive the file it opened and misclassify that file's END_FILE as superseded).
        if (load_.gen && (load_.started || load_.reply_seen)) {
            AppendLog("load complete gen=" + std::to_string(load_.gen) + " origin=" + load_.origin + " entry=" +
                      std::to_string(load_.entry_id) + (load_.started ? std::string() : std::string(" (start not matched)")) +
                      " since_open_ms=" + Number(MsSince(load_.at), 1));
            load_ = {};
        }
        file_loaded_ = true;
        ended_emitted_ = false;
        pending_open_ = false;
        first_frame_pending_ = true;
        {
            const std::string hwdec_at_load = PropertyString("hwdec-current");
            AppendLog("opened engine=mpv duration_ms=" + std::to_string(
                static_cast<std::int64_t>(std::llround(PropertyDouble("duration", 0.0) * 1000.0))) +
                " hwdec_current=" + (hwdec_at_load.empty() ? std::string("pending") : hwdec_at_load) +
                " since_open_ms=" + Number(MsSince(open_requested_at_), 1));
        }
        // The child stays hidden (or keeps the previous image) until PLAYBACK_RESTART: showing it
        // here put an empty black window on screen for the whole decoder/filter start-up.
        EmitOpened();
        EmitState(); // "playing" here means decoding started, not that a frame is visible
        EmitPosition(true);
        // The chain was built in on_preloaded; now that the decoder exists, change only what differs.
        ApplyFileEnhancement(false);
        if (first_frame_pending_ && host_ && IsWindow(host_))
            SetTimer(host_, kFirstFrameTimerId, kFirstFrameSafetyMs, [](HWND, UINT, UINT_PTR, DWORD) {
                if (g_timer_backend) g_timer_backend->OnFirstFrameTimeout();
            });
        break;
    case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* property = static_cast<mpv_event_property*>(event->data);
        // Before the handlers: a mid-stream format change with a VS stage enters the busy state first (7.3b).
        if (property && property->name && std::strcmp(property->name, "video-params") == 0) OnVideoParamsEvent(property);
        CacheObservedProperty(property);
        if (property && property->name) OnProperty(property->name);
        break;
    }
    case MPV_EVENT_VIDEO_RECONFIG:
        // Audit 7.3b: during a reconfig busy period this is the filter-output reconfiguration, emitted after the script
        // reload produced its first frame (the input-side one came before the video-params notification that began the
        // period). The barrier then proves nothing queued behind it holds the core. Ignored otherwise.
        if (vs_busy_ && vs_busy_trigger_ == "reconfig") {
            AppendLog("vs-init reconfig video-reconfig after " + Number(MsSince(vs_busy_since_), 0) + " ms (watch probes=" +
                      std::to_string(reconfig_watch_probes_) + " longest_wait_ms=" + Number(reconfig_watch_max_ms_, 0) + ")");
            RequestVsBusyEnd("video-reconfig", true);
        }
        break;
    case MPV_EVENT_SEEK:
        // mpv reloads a vapoursynth filter's script on every seek (engine deserialise under TensorRT): Seek() set
        // vs_busy_ already for our own seeks; this covers seeks issued any other way (chapter, loop, LAN remote, the
        // refresh seek after an hwdec change). While busy, the seek now owns the end: only its PLAYBACK_RESTART may end
        // the busy period (a pending end from this drain, a running barrier or the apply probe no longer can).
        if (vs_busy_) {
            ++vs_seek_events_;
            vs_restart_pending_ = true;
            if (!vs_end_pending_by_.empty() || vs_probe_kind_ != 0) {
                AppendLog("vs-init seek event while ending (pending=" + (vs_end_pending_by_.empty() ? std::string("-") : vs_end_pending_by_) +
                          " probe=" + std::to_string(vs_probe_kind_) + "); waiting for its playback-restart, seeks=" +
                          std::to_string(vs_seek_events_) + "/" + std::to_string(vs_seek_cmds_));
                vs_end_pending_by_.clear();
                vs_barrier_by_.clear();
                vs_probe_id_ = 0;
                vs_probe_kind_ = 0;
            }
        } else if (file_loaded_ && VsStageInVf()) {
            BeginVsBusy("seek");
            ++vs_seek_events_;
            vs_restart_pending_ = true;
        }
        ResetRateWindow("seek");
        EmitState("seeking");
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        // The first frame after the load / seek / refresh left the filter chain: the VapourSynth script is loaded.
        // Completed after this drain (or after the probe barrier when a seek command is still unaccounted for).
        vs_restart_pending_ = false;
        RequestVsBusyEnd("playback-restart");
        ResetRateWindow("restart");
        RecordShaderCompileMeasurement();
        // Fallback in case the hwdec-current change notification is missed: by
        // the first playback restart a frame has been decoded, so the value is final.
        RefreshOpenedHwdec();
        // mpv sends this once the first frame after a load/seek was handed to the VO and waited
        // for ("first video frame after restart shown" is logged just before, at verbose level).
        if (first_frame_pending_) PresentFirstFrame("playback-restart");
        EmitState();
        EmitPosition(true);
        break;
    case MPV_EVENT_END_FILE: {
        const auto* ended = static_cast<mpv_event_end_file*>(event->data);
        file_loaded_ = false;
        // The file (and its filter chain) is gone; a following file sets vs_busy_ again in on_preloaded.
        EndVsBusy("end-file");
        first_frame_pending_ = false;
        if (host_) KillTimer(host_, kFirstFrameTimerId);
        const bool failed = ended && ended->reason == MPV_END_FILE_REASON_ERROR;
        // 2B: classify the entry against the current load request (its reply named the entry it created).
        const std::int64_t ended_entry = ended ? ended->playlist_entry_id : 0;
        if (load_.gen) {
            const bool current = load_.reply_seen && load_.entry_id > 0 && ended_entry == load_.entry_id;
            if (!current) {
                // The file a loadfile replaces, or an older request's entry: never reported against the new title.
                AppendLog("ended entry=" + std::to_string(ended_entry) + " reason=" + std::to_string(ended ? ended->reason : -1) +
                          (failed ? " error=" + MpvErrorText(api_ ? api_->error_string : nullptr, ended->error) : std::string()) +
                          " (superseded by load gen=" + std::to_string(load_.gen) + "; surface kept, not reported)");
                EmitState("opening");
                break;
            }
            if (ended->reason == MPV_END_FILE_REASON_REDIRECT && ended->playlist_insert_num_entries > 0) {
                AppendLog("ended entry=" + std::to_string(ended_entry) + " reason=redirect -> entry=" + std::to_string(ended->playlist_insert_id) +
                          " (" + std::to_string(ended->playlist_insert_num_entries) + " entries)");
                load_.entry_id = ended->playlist_insert_id;
                load_.started = false;
                EmitState("opening");
                break;
            }
            if (failed || ended->reason == MPV_END_FILE_REASON_REDIRECT) {
                const int code = failed ? ended->error : MPV_ERROR_NOTHING_TO_PLAY;
                FailLoad("MPV_END_FILE", LoadErrorText(code), code, "end-file", false);
                break;
            }
            load_ = {}; // stop / quit / eof of the current entry: the branches below
        } else if (failed && stop_requested_) {
            // A failure racing the user's stop (or the stop of a superseded load): cancelled, not an error.
            AppendLog("ended reason=error after a stop (" + MpvErrorText(api_ ? api_->error_string : nullptr, ended->error) +
                      "; cancelled, not reported)");
            stop_requested_ = false;
            EmitState("stopped");
            break;
        }
        // pending_open_: this END_FILE belongs to the file a `loadfile replace` is replacing.
        const bool switching = pending_open_ && !failed;
        // A switch keeps the old image until the new first frame; a requested stop keeps the
        // (now black) surface until the UI's releaseSurface; errors and anything else hide it.
        if (!switching && !(stop_requested_ && !failed) && video_child_ && IsWindow(video_child_))
            ShowWindow(video_child_, SW_HIDE);
        if (failed) {
            pending_open_ = false;
            stop_requested_ = false;
            AppendLog("ended reason=error");
            // A playing file / stream failed (no load request): reported once, latched like a load failure (2B).
            if (!load_error_latched_) {
                EmitError("MPV_END_FILE", MpvErrorText(api_ ? api_->error_string : nullptr, ended->error), true);
                EmitState("error");
                load_error_latched_ = true;
            }
        } else if (switching) {
            AppendLog("ended reason=replaced (switch; surface kept, no stopped state)");
            EmitState("opening");
        } else if (ended && ended->reason == MPV_END_FILE_REASON_EOF) {
            AppendLog("ended reason=eof");
            if (!ended_emitted_) Emit("ended", "{}");
            ended_emitted_ = true;
            EmitState("ended");
        } else {
            AppendLog(std::string("ended reason=stopped") + (stop_requested_ ? " surface=kept" : ""));
            stop_requested_ = false;
            EmitState("stopped");
        }
        break;
    }
    case MPV_EVENT_SHUTDOWN:
        file_loaded_ = false;
        EndVsBusy("shutdown");
        break;
    default:
        break;
    }
}

void MpvBackend::OnProperty(const char* name) {
    if (!name) return;
    if (std::strcmp(name, "time-pos") == 0) RecordShaderCompileMeasurement();
    // Playback-clock discontinuities for the frameProcessing rate window (the frozen states are also checked per tick).
    if (std::strcmp(name, "pause") == 0 || std::strcmp(name, "seeking") == 0 || std::strcmp(name, "paused-for-cache") == 0 ||
        std::strcmp(name, "speed") == 0 || std::strcmp(name, "vf") == 0)
        ResetRateWindow(name);
    if (std::strcmp(name, "pause") == 0 || std::strcmp(name, "seeking") == 0 ||
        std::strcmp(name, "core-idle") == 0 || std::strcmp(name, "paused-for-cache") == 0 ||
        std::strcmp(name, "eof-reached") == 0 || std::strcmp(name, "idle-active") == 0) {
        if (std::strcmp(name, "eof-reached") == 0 && PropertyString("eof-reached") == "yes") {
            if (!ended_emitted_) Emit("ended", "{}");
            ended_emitted_ = true;
            EmitState("ended");
        } else EmitState();
    }
    else if (std::strcmp(name, "time-pos") == 0 || std::strcmp(name, "duration") == 0 ||
             std::strcmp(name, "estimated-vf-fps") == 0 || std::strcmp(name, "frame-drop-count") == 0 ||
             std::strcmp(name, "decoder-frame-drop-count") == 0 || std::strcmp(name, "avsync") == 0)
        EmitPosition();
    else if (std::strcmp(name, "hwdec-current") == 0) RefreshOpenedHwdec();
    else if (std::strcmp(name, "track-list") == 0 && file_loaded_) EmitTracks();
    else if (std::strcmp(name, "sid") == 0 && file_loaded_) EmitSubtitleTrack();
    else if (std::strcmp(name, "aid") == 0 && file_loaded_) {
        AppendLog("audio track selected aid=" + PropertyString("aid"));
        EmitOpened();
    }
    const bool fps_property = std::strcmp(name, "container-fps") == 0 || std::strcmp(name, "display-fps") == 0;
    // Bit-perfect / vsync_auto policy follows the source and display rates (window moved to a 60 Hz display, a
    // variable-fps source): only video-sync / interpolation are written, both live; no vf rebuild.
    if (file_loaded_ && fps_property && !applying_round6_enhancement_) ApplyAudioSyncPolicy(name);
    if (std::strcmp(name, "aid") == 0) {
        const std::string aid = PropertyString("aid");
        if (!aid.empty() && aid != "no" && aid != "auto") last_aid_ = aid;
    }
    if (file_loaded_ && fps_property && !applying_round6_enhancement_ &&
        video_config_["interpolation"]["factor"].str("auto") == "auto") {
        const std::string mode = video_config_["interpolation"]["mode"].str();
        if (mode == "mvtools" || mode == "rife" || mode == "drba") {
            const double source_fps = PropertyDouble("container-fps", 0.0);
            const double display_hz = EffectiveDisplayHz();
            const bool changed = std::abs(source_fps - round6_last_auto_source_fps_) > 0.01 ||
                std::abs(display_hz - round6_last_auto_display_hz_) > 0.05;
            if (changed && source_fps > 0.0 && display_hz > 0.0) {
                round6_last_auto_source_fps_ = source_fps;
                round6_last_auto_display_hz_ = display_hz;
                applying_round6_enhancement_ = true;
                ApplyEnhancement(video_config_);
                applying_round6_enhancement_ = false;
            }
        }
    }
    // Upscale H_Pre auto = min(source height, ceil(H_Max / scale)). The chain is usually built in on_preloaded from the
    // demuxer's height (or none); once the decoder reports its height, rebuild only if that changes the resolved H_Pre
    // (e.g. a 480p source: 480 instead of 720). Same re-apply path as the fps change above.
    if (file_loaded_ && std::strcmp(name, "video-params") == 0 && !applying_round6_enhancement_ && round6_upscale_pre_auto_) {
        const int height = static_cast<int>(PropertyDouble("video-params/h", 0.0));
        if (height > 0 && height != round6_upscale_pre_source_height_) {
            const int resolved = AutoUpscalePreHeight(height, round6_upscale_pre_hmax_, round6_upscale_scale_);
            if (resolved != round6_upscale_pre_height_) {
                AppendLog("r6-upscale h_pre auto re-evaluated src=" + std::to_string(round6_upscale_pre_source_height_) + "->" +
                          std::to_string(height) + " h_pre=" + std::to_string(round6_upscale_pre_height_) + "->" + std::to_string(resolved));
                applying_round6_enhancement_ = true;
                ApplyEnhancement(video_config_);
                applying_round6_enhancement_ = false;
            } else {
                round6_upscale_pre_source_height_ = height;
            }
        }
    }
    // The HDR source is known once the decoder reports video-params (normally already in the FILE_LOADED pass): when
    // its class differs from what the last pass applied, re-apply only the HDR options (no vf / shader change).
    // During a 7.3b reconfig period (not SyncAllowed) this is skipped; FinishVsBusy re-runs it when the chain is up.
    if (file_loaded_ && std::strcmp(name, "video-params") == 0 && !applying_round6_enhancement_ && SyncAllowed())
        RecheckHdrSource("video-params");
    // 3A: mpv's auto deinterlacer turned on for this file (decided at the first decoded frame, again at splices).
    if (std::strcmp(name, "deinterlace-active") == 0) {
        const bool active = ObservedFlag("deinterlace-active");
        const std::string value = active ? "yes" : "no";
        if (value != deint_active_logged_) {
            deint_active_logged_ = value;
            const auto cached_fps = observed_cache_.find("container-fps"); // observed: no libmpv call here
            const double container = cached_fps != observed_cache_.end() && cached_fps->second.has_number ? cached_fps->second.number : 0.0;
            const bool vs_interp = round6_interpolation_active_ && (round6_interpolation_mode_ == "mvtools" ||
                                   round6_interpolation_mode_ == "rife" || round6_interpolation_mode_ == "drba");
            AppendLog("deinterlace-active=" + value + " container-fps=" + Number(container) +
                      (active ? " vs_input_fps=" + Number(container * 2.0) + " (field rate)" : std::string()) +
                      (active && !deint_seen_ && vs_interp ? "; VapourSynth interpolation was composed for the container rate: recomposition pending" : ""));
            if (active && !deint_seen_ && vs_interp) vs_rate_reapply_pending_ = true;
            if (active) deint_seen_ = true; // sticky for this file
        }
    }
    if (file_loaded_ && (std::strcmp(name, "time-pos") == 0 || std::strcmp(name, "video-params") == 0 ||
                         std::strcmp(name, "video-out-params") == 0 || fps_property)) {
#if defined(VP_FAULT_INJECTION)
        const auto fp_started = std::chrono::steady_clock::now();
        EmitFrameProcessing();
        if (fp_phase_ != 0) g_fp_tick_acc += MsSince(fp_started);
#else
        EmitFrameProcessing();
#endif
    }
}

void MpvBackend::Emit(const char* name, const std::string& json) {
    // Last copies for the diagnostics export (state.json); Emit runs on the UI thread only.
    if (std::strcmp(name, "frameProcessing") == 0) last_frame_processing_json_ = json;
    else if (std::strcmp(name, "opened") == 0) last_opened_json_ = json;
    if (event_) event_(name, json);
}

void MpvBackend::EmitState(const char* forced) {
    // 2B: a reported load / playback failure stays on screen until the next Open / Stop / device-loss reload
    // (mpv's idle-active / core-idle changes after it used to turn the UI to "stopped" and drop the message).
    if (load_error_latched_ && !(forced && std::strcmp(forced, "error") == 0)) return;
    std::string state;
    if (forced) state = forced;
    else if (!file_loaded_) state = PropertyString("idle-active") == "yes" ? "stopped" : "opening";
    else if (PropertyString("eof-reached") == "yes") state = "ended";
    else if (PropertyString("seeking") == "yes") state = "seeking";
    else state = PropertyString("pause") == "yes" ? "paused" : "playing";
    if (state == last_state_) return;
    last_state_ = state;
    Emit("state", "{\"state\":" + Json::Quote(state) + "}");
}

std::int64_t MpvBackend::PositionMs() {
    return static_cast<std::int64_t>(std::llround(PropertyDouble("time-pos", 0.0) * 1000.0));
}

void MpvBackend::EmitPosition(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && last_position_emit_.time_since_epoch().count() != 0 &&
        now - last_position_emit_ < std::chrono::milliseconds(250)) return;
    last_position_emit_ = now;
    const double duration = PropertyDouble("duration", 0.0);
    const double fps = PropertyDouble("estimated-vf-fps", PropertyDouble("container-fps", 0.0));
    const double dropped = PropertyDouble("frame-drop-count", 0.0) + PropertyDouble("decoder-frame-drop-count", 0.0);
    const double avsync = PropertyDouble("avsync", 0.0);
    const std::string json = "{\"position_ms\":" + std::to_string(PositionMs()) +
        ",\"duration_ms\":" + std::to_string(static_cast<std::int64_t>(std::llround(duration * 1000.0))) +
        ",\"fps\":" + Number(fps) + ",\"dropped\":" + Number(dropped, 0) +
        ",\"av_offset_ms\":" + Number(avsync * 1000.0) + ",\"clock\":\"wasapi\"}";
    Emit("position", json);
}

std::string MpvBackend::BuildTrackListJson(const char* wanted_type, const char* array_name) {
    mpv_node value{};
    if (!ctx_ || !api_ || !initialized_ || GetPropertySync("track-list", MPV_FORMAT_NODE, &value) < 0)
        return std::string("\"") + array_name + "\":[]";
    std::string json = std::string("\"") + array_name + "\":[";
    bool first = true;
    if (value.format == MPV_FORMAT_NODE_ARRAY && value.u.list) {
        for (int i = 0; i < value.u.list->num; ++i) {
            const mpv_node* item = &value.u.list->values[i];
            if (NodeString(MapValue(item, "type")) != wanted_type) continue;
            const auto id = static_cast<std::int64_t>(NodeNumber(MapValue(item, "id"), -1));
            if (id < 0) continue;
            if (!first) json += ',';
            first = false;
            json += "{\"index\":" + std::to_string(id) + ",\"id\":" + std::to_string(id) +
                ",\"language\":" + Json::Quote(NodeString(MapValue(item, "lang"))) +
                ",\"title\":" + Json::Quote(NodeString(MapValue(item, "title"))) +
                ",\"codec\":" + Json::Quote(NodeString(MapValue(item, "codec"))) +
                ",\"selected\":" + std::string(NodeBool(MapValue(item, "selected")) ? "true" : "false") +
                ",\"default\":" + std::string(NodeBool(MapValue(item, "default")) ? "true" : "false") +
                ",\"forced\":" + std::string(NodeBool(MapValue(item, "forced")) ? "true" : "false") +
                ",\"bitmap\":" + std::string(NodeBool(MapValue(item, "image")) ? "true" : "false") +
                ",\"external\":" + std::string(NodeBool(MapValue(item, "external")) ? "true" : "false") +
                ",\"path\":" + Json::Quote(NodeString(MapValue(item, "external-filename"))) + "}";
        }
    }
    api_->free_node_contents(&value);
    json += ']';
    return json;
}

std::string MpvBackend::BuildChaptersJson() {
    mpv_node value{};
    if (!ctx_ || !api_ || !initialized_ || GetPropertySync("chapter-list", MPV_FORMAT_NODE, &value) < 0) return "[]";
    std::string json = "[";
    bool first = true;
    const int count = value.format == MPV_FORMAT_NODE_ARRAY && value.u.list ? value.u.list->num : 0;
    const double duration_ms = PropertyDouble("duration", 0.0) * 1000.0;
    if (count > 0) {
        for (int i = 0; i < count; ++i) {
            const mpv_node* item = &value.u.list->values[i];
            const auto start_ms = static_cast<std::int64_t>(std::llround(NodeNumber(MapValue(item, "time")) * 1000.0));
            const std::int64_t end_ms = i + 1 < count
                ? static_cast<std::int64_t>(std::llround(NodeNumber(MapValue(&value.u.list->values[i + 1], "time")) * 1000.0))
                : static_cast<std::int64_t>(std::llround(duration_ms));
            if (!first) json += ',';
            first = false;
            json += "{\"start_ms\":" + std::to_string(start_ms) + ",\"end_ms\":" + std::to_string(end_ms) + ",\"title\":" +
                Json::Quote(NodeString(MapValue(item, "title"))) + "}";
        }
    }
    api_->free_node_contents(&value);
    return json + ']';
}

std::string MpvBackend::BuildAudioInfoJson() {
    mpv_node value{};
    int channels = 0, rate = 0, bits = 0;
    int output_rate = 0;
    std::string codec = PropertyString("audio-codec-name");
    if (ctx_ && api_ && initialized_ && GetPropertySync("audio-params", MPV_FORMAT_NODE, &value) >= 0) {
        if (value.format == MPV_FORMAT_NODE_MAP) {
            channels = static_cast<int>(NodeNumber(MapValue(&value, "channel-count")));
            rate = static_cast<int>(NodeNumber(MapValue(&value, "samplerate")));
            const std::string format = Lower(NodeString(MapValue(&value, "format")));
            if (format.find("double") != std::string::npos || format.find("dbl") != std::string::npos || format.find("64") != std::string::npos) bits = 64;
            else if (format.find("float") != std::string::npos || format.find("flt") != std::string::npos || format.find("32") != std::string::npos) bits = 32;
            else if (format.find("24") != std::string::npos) bits = 24;
            else if (format.find("16") != std::string::npos) bits = 16;
            else if (format.find("8") != std::string::npos) bits = 8;
        }
        api_->free_node_contents(&value);
    }
    if (ctx_ && api_ && initialized_ && GetPropertySync("audio-out-params", MPV_FORMAT_NODE, &value) >= 0) {
        if (value.format == MPV_FORMAT_NODE_MAP)
            output_rate = static_cast<int>(NodeNumber(MapValue(&value, "samplerate")));
        api_->free_node_contents(&value);
    }
    if (codec.empty() || rate == 0) return "null";
    return "{\"codec\":" + Json::Quote(codec) + ",\"channels\":" + std::to_string(channels) +
        ",\"bits\":" + std::to_string(bits) + ",\"rate\":" + std::to_string(rate) +
        ",\"output_rate\":" + std::to_string(output_rate > 0 ? output_rate : rate) + "}";
}

// Audio output for the info panel: output API, device name and output format. The device
// description is looked up in audio-device-list only when the selected device id changes.
std::string MpvBackend::AudioOutputJson() {
    const std::string device = PropertyString("audio-device");
    if (device != audio_out_device_id_) {
        audio_out_device_id_ = device;
        audio_out_device_desc_.clear();
        NodeHolder list_holder(api_ ? api_->free_node_contents : nullptr);
        const mpv_node* list = ctx_ && api_ && initialized_ ? ReadNode("audio-device-list", list_holder) : nullptr;
        if (list && list->format == MPV_FORMAT_NODE_ARRAY && list->u.list)
            for (int i = 0; i < list->u.list->num; ++i)
                if (NodeString(MapValue(&list->u.list->values[i], "name")) == device) {
                    audio_out_device_desc_ = NodeString(MapValue(&list->u.list->values[i], "description"));
                    break;
                }
    }
    std::string format, channels;
    NodeHolder out_holder(api_ ? api_->free_node_contents : nullptr);
    const mpv_node* out = ctx_ && api_ && initialized_ ? ReadNode("audio-out-params", out_holder) : nullptr;
    if (out && out->format == MPV_FORMAT_NODE_MAP) {
        format = NodeString(MapValue(out, "format"));
        channels = NodeString(MapValue(out, "hr-channels"));
    }
    bool bitperfect = false;
    const std::string reason = BitPerfectReason(&bitperfect);
    const std::string verdict = bitperfect ? std::string("true") : "false " + reason;
    if (verdict != bitperfect_logged_) {
        bitperfect_logged_ = verdict;
        AppendLog("bitperfect-check " + verdict + " requested=" + (audio_exclusive_ == "yes" ? "yes" : "no"));
    }
    // Actual resampling (not the policy's intent): logged on change with the clock that produced it.
    const std::string resampler = AudioResamplerState();
    const std::string sync_now = PropertyString("video-sync");
    const std::string effective_clock = sync_now.rfind("display-", 0) == 0 ? "display" : "audio";
    const std::string resampler_line = "audio-resampler state=" + resampler + " video-sync=" + sync_now + " clock=" + master_clock_ +
        " effective_clock=" + effective_clock + " exclusive=" + PropertyString("audio-exclusive");
    if (resampler_line != resampler_logged_) {
        resampler_logged_ = resampler_line;
        AppendLog(resampler_line + " speed_correction=" + Number(PropertyDouble("audio-speed-correction", 1.0), 5));
    }
    return "{\"ao\":" + Json::Quote(PropertyString("current-ao")) + ",\"device\":" + Json::Quote(device) +
        ",\"device_desc\":" + Json::Quote(audio_out_device_desc_) + ",\"format\":" + Json::Quote(format) +
        ",\"channels\":" + Json::Quote(channels) +
        // Actual AO state (not the config), the level, and the bit-perfect verdict with the first failing check.
        ",\"exclusive\":" + std::string(PropertyString("audio-exclusive") == "yes" ? "true" : "false") +
        ",\"bitperfect_requested\":" + std::string(audio_exclusive_ == "yes" ? "true" : "false") +
        ",\"bitperfect\":" + std::string(bitperfect ? "true" : "false") + ",\"bitperfect_reason\":" + Json::Quote(reason) +
        ",\"volume\":" + Number(PropertyDouble("volume", 100.0), 0) + ",\"saved_volume\":" + std::to_string(audio_volume_) +
        ",\"mute\":" + std::string(PropertyString("mute") == "yes" ? "true" : "false") +
        ",\"video_sync\":" + Json::Quote(sync_now) +
        ",\"master_clock\":" + Json::Quote(master_clock_) + ",\"effective_clock\":" + Json::Quote(effective_clock) +
        ",\"resampler\":" + Json::Quote(resampler) +
        ",\"policy_reason\":" + Json::Quote(audio_policy_reason_) + "}";
}

// Source format details for the info panel: selected video/audio track (codec, profile, container
// bitrate, channel layout), decoded pixel format and colour, container and file size.
std::string MpvBackend::BuildMediaInfoJson() {
    std::string video_json = "null", audio_json = "null";
    mpv_node tracks{};
    if (ctx_ && api_ && initialized_ && GetPropertySync("track-list", MPV_FORMAT_NODE, &tracks) >= 0) {
        if (tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list) {
            for (int i = 0; i < tracks.u.list->num; ++i) {
                const mpv_node* track = &tracks.u.list->values[i];
                if (!NodeBool(MapValue(track, "selected"))) continue;
                const std::string type = NodeString(MapValue(track, "type"));
                const auto text = [&](const char* key) { return Json::Quote(NodeString(MapValue(track, key))); };
                const auto number = [&](const char* key) { return Number(NodeNumber(MapValue(track, key)), 3); };
                if (type == "video" && video_json == "null") {
                    video_json = "{\"codec\":" + text("codec") + ",\"codec_desc\":" + text("codec-desc") + ",\"profile\":" + text("codec-profile") +
                        ",\"demux_bitrate\":" + number("demux-bitrate") + ",\"demux_fps\":" + number("demux-fps") + "}";
                } else if (type == "audio" && audio_json == "null") {
                    audio_json = "{\"codec\":" + text("codec") + ",\"codec_desc\":" + text("codec-desc") + ",\"profile\":" + text("codec-profile") +
                        ",\"channels\":" + number("demux-channel-count") + ",\"layout\":" + text("demux-channels") +
                        ",\"samplerate\":" + number("demux-samplerate") + ",\"demux_bitrate\":" + number("demux-bitrate") +
                        ",\"lang\":" + text("lang") + ",\"title\":" + text("title") + "}";
                }
            }
        }
        api_->free_node_contents(&tracks);
    }
    std::string pixel = "{}";
    mpv_node params{};
    if (ctx_ && api_ && initialized_ && GetPropertySync("video-params", MPV_FORMAT_NODE, &params) >= 0) {
        if (params.format == MPV_FORMAT_NODE_MAP) {
            const auto text = [&](const char* key) { return Json::Quote(NodeString(MapValue(&params, key))); };
            pixel = "{\"pixelformat\":" + text("pixelformat") + ",\"hw_pixelformat\":" + text("hw-pixelformat") +
                ",\"colormatrix\":" + text("colormatrix") + ",\"primaries\":" + text("primaries") + ",\"gamma\":" + text("gamma") +
                ",\"levels\":" + text("colorlevels") + ",\"sig_peak\":" + Number(NodeNumber(MapValue(&params, "sig-peak")), 3) +
                ",\"average_bpp\":" + Number(NodeNumber(MapValue(&params, "average-bpp")), 0) + "}";
        }
        api_->free_node_contents(&params);
    }
    return "{\"video\":" + video_json + ",\"audio\":" + audio_json + ",\"pixel\":" + pixel +
        ",\"container\":" + Json::Quote(PropertyString("file-format")) +
        ",\"file_size\":" + Number(PropertyDouble("file-size", 0.0), 0) + ",\"hdr\":" + HdrStateJson() + "}";
}

// Called only through EmitOpened, i.e. while SyncAllowed() (HasTrackType below reads track-list directly).
std::string MpvBackend::BuildOpenedJson() {
    mpv_node video{};
    int width = static_cast<int>(PropertyDouble("width", 0));
    int height = static_cast<int>(PropertyDouble("height", 0));
    if (ctx_ && api_ && initialized_ && GetPropertySync("video-params", MPV_FORMAT_NODE, &video) >= 0) {
        width = static_cast<int>(NodeNumber(MapValue(&video, "w"), width));
        height = static_cast<int>(NodeNumber(MapValue(&video, "h"), height));
        api_->free_node_contents(&video);
    }
    // At MPV_EVENT_FILE_LOADED the video decoder may not exist yet, so
    // hwdec-current is still empty. Report "pending" (not software) and re-emit
    // opened once mpv settles the value (see RefreshOpenedHwdec).
    const std::string hwdec = PropertyString("hwdec-current");
    const bool pending = hwdec.empty();
    const bool software = !pending && (hwdec == "no" || hwdec == "none");
    if (pending && opened_hwdec_.empty() && file_loaded_) AppendLog("opened sent with hwdec_current=pending");
    opened_hwdec_ = hwdec;
    const std::string codec = PropertyString("video-codec-name");
    const double fps = PropertyDouble("container-fps", 0.0);
    const bool has_audio = HasTrackType(ctx_, api_ ? api_->get_property : nullptr,
                                        api_ ? api_->free_node_contents : nullptr, "audio");
    return "{\"path\":" + Json::Quote(current_path_) + ",\"engine\":\"mpv\",\"subtitleRenderer\":\"engine\"" +
        ",\"duration_ms\":" + std::to_string(static_cast<std::int64_t>(std::llround(PropertyDouble("duration", 0.0) * 1000.0))) +
        ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
        ",\"video_codec\":" + Json::Quote(codec) + ",\"fps\":" + Number(fps) +
        ",\"has_audio\":" + std::string(has_audio ? "true" : "false") +
        ",\"adapter\":" + Json::Quote(active_adapter_.empty() ? "D3D11 (default adapter)" : active_adapter_) +
        ",\"software_decode\":" + std::string(software ? "true" : "false") +
        ",\"hwdec_current\":" + Json::Quote(pending ? "pending" : software ? "software" : hwdec) + ",\"clock\":\"wasapi\",\"audio\":" + BuildAudioInfoJson() +
        "," + BuildTrackListJson("video", "videoTracks") + "," + BuildTrackListJson("sub", "subtitles") + "," + BuildTrackListJson("audio", "audioTracks") +
        ",\"chapters\":" + BuildChaptersJson() + ",\"media\":" + BuildMediaInfoJson() + "}";
}

void MpvBackend::EmitOpened() {
    // BuildOpenedJson needs a dozen synchronous reads (track-list, video-params, audio-params, chapters, ...). While a
    // VapourSynth chain initialises they would wait for it, so the event is sent once the chain is up (EndVsBusy); the
    // UI keeps its opening state (and the vsInit overlay) meanwhile.
    if (!SyncAllowed()) {
        vs_deferred_opened_ = true;
        return;
    }
    Emit("opened", BuildOpenedJson());
}

void MpvBackend::RefreshOpenedHwdec() {
    // Re-send opened only when an earlier opened went out with a different
    // hwdec-current (normally "pending"); the UI decides the software-decode
    // toast and the info panel's decoder row from this event.
    if (!file_loaded_) return;
    const std::string hwdec = PropertyString("hwdec-current");
    if (hwdec.empty() || hwdec == opened_hwdec_) return;
    AppendLog("hwdec-current resolved: " + hwdec);
    EmitOpened();
}

void MpvBackend::EmitTracks() {
    // Refresh the track menu and selected subtitle state after an external
    // subtitle is added or a track is selected. The opened event is also the UI's
    // canonical source for the subtitle/audio track arrays.
    EmitOpened();
    EmitSubtitleTrack();
}

void MpvBackend::EmitSubtitleTrack() {
    if (!SyncAllowed()) { // needs track-list (external file name); sent from EndVsBusy
        vs_deferred_subtitle_ = true;
        return;
    }
    const std::string sid = PropertyString("sid");
    const bool off = sid.empty() || sid == "no" || sid == "auto";
    std::string source = off ? "off" : "embedded";
    std::string path;
    if (!off && ctx_) {
        mpv_node tracks{};
        if (GetPropertySync("track-list", MPV_FORMAT_NODE, &tracks) >= 0) {
            if (tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list) {
                for (int i = 0; i < tracks.u.list->num; ++i) {
                    const mpv_node* item = &tracks.u.list->values[i];
                    if (NodeString(MapValue(item, "type")) != "sub" ||
                        std::to_string(static_cast<std::int64_t>(NodeNumber(MapValue(item, "id"), -1))) != sid) continue;
                    if (NodeBool(MapValue(item, "external"))) {
                        source = "external";
                        path = NodeString(MapValue(item, "external-filename"));
                    }
                    break;
                }
            }
            api_->free_node_contents(&tracks);
        }
    }
    Emit("subtitle", "{\"action\":\"track\",\"source\":" + Json::Quote(source) +
         ",\"index\":" + (off ? "-1" : sid) + ",\"path\":" + Json::Quote(path) + "}");
}

void MpvBackend::EmitError(const std::string& code, const std::string& message, bool fatal) {
    // ApplyEnhancement runs in on_preloaded and again after FILE_LOADED; report a non-fatal
    // problem once per file instead of toasting it twice.
    if (!fatal) {
        const std::string key = code + "|" + message;
        if (key == last_nonfatal_error_) return;
        last_nonfatal_error_ = key;
    }
    Emit("error", "{\"fatal\":" + std::string(fatal ? "true" : "false") + ",\"code\":" + Json::Quote(code) +
         ",\"message\":" + Json::Quote(message) + "}");
}

void MpvBackend::EmitFrameProcessing() {
    if (!ctx_ || !initialized_ || !file_loaded_ || !event_) return;
    // Skipped while a VapourSynth chain initialises (nothing changes meanwhile: no frames); FinishVsBusy asks again.
    // Also inside BuildFrameProcessing (the emit in progress is that request; a new one would clear its snapshot).
    if (!SyncAllowed() || fp_snapshot_active_) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_frame_processing_emit_.time_since_epoch().count() != 0 &&
        now - last_frame_processing_emit_ < std::chrono::seconds(1)) return;
    last_frame_processing_emit_ = now;
    // A request still in flight is replaced: the new one is built from the state after whatever asked for this emit.
    if (fp_phase_ != 0 && now - fp_requested_at_ >= std::chrono::seconds(1))
        AppendLog("frameProcessing snapshot replaced after " + Number(MsSince(fp_requested_at_), 0) + " ms (" +
                  std::to_string(fp_requests_.size()) + " replies outstanding)");
    RequestFrameProcessingSnapshot();
}

void MpvBackend::RequestFrameProcessingSnapshot() {
    ++fp_generation_;
    fp_requests_.clear(); // replies to a replaced request are ignored (OnFrameProcessingReply)
    fp_snapshot_.clear();
    fp_request_epoch_ = fp_epoch_;
    fp_requested_at_ = std::chrono::steady_clock::now();
    fp_phase_ = 1;
    int index = 0;
    for (const FpRequest& request : kFpRequests) SendFrameProcessingRequest(request.kind, request.name, index++);
    if (observed_cache_.find("display-names") == observed_cache_.end()) SendFrameProcessingRequest('S', "display-names", index++);
    if (fp_requests_.empty()) OnFrameProcessingSnapshotComplete(); // every request refused (event queue full)
}

void MpvBackend::SendFrameProcessingRequest(char kind, const char* name, int index) {
    const std::string key = std::string(1, kind) + name;
    fp_snapshot_[key] = FpSnapshotValue{}; // answers like a failed read unless the reply brings a value
    const std::uint64_t id = kFpReplyBase + (fp_generation_ << 8) + static_cast<std::uint64_t>(index & 0xff);
    const mpv_format format = kind == 'N' ? MPV_FORMAT_NODE : kind == 'D' ? MPV_FORMAT_DOUBLE : MPV_FORMAT_STRING;
    if (api_->get_property_async(ctx_, id, name, format) >= 0) fp_requests_[id] = key;
}

void MpvBackend::OnFrameProcessingReply(std::uint64_t id, int error, const void* property_event) {
    const auto request = fp_requests_.find(id);
    if (request == fp_requests_.end()) return; // a replaced request
    FpSnapshotValue& value = fp_snapshot_[request->second];
    fp_requests_.erase(request);
    value.at = std::chrono::steady_clock::now();
    const auto* property = static_cast<const mpv_event_property*>(property_event);
    if (error >= 0 && property && property->data) {
        switch (property->format) {
        case MPV_FORMAT_STRING: {
            const char* text = *static_cast<char* const*>(property->data);
            value.text = text ? text : "";
            value.ok = true;
            break;
        }
        case MPV_FORMAT_DOUBLE:
            value.number = *static_cast<const double*>(property->data);
            value.ok = true;
            break;
        case MPV_FORMAT_NODE:
            value.node = std::make_shared<const OwnedNode>(*static_cast<const mpv_node*>(property->data));
            value.ok = true;
            break;
        default:
            break;
        }
    }
    if (fp_requests_.empty()) OnFrameProcessingSnapshotComplete();
}

void MpvBackend::OnFrameProcessingSnapshotComplete() {
    if (fp_phase_ == 1) {
        // AudioOutputJson looks the device description up only when the selected device changed: one more request.
        const FpSnapshotValue* device = FpSnapshotFind('S', "audio-device");
        if ((device && device->ok ? device->text : std::string()) != audio_out_device_id_) {
            fp_phase_ = 2;
            SendFrameProcessingRequest('N', "audio-device-list", 0xff);
            if (!fp_requests_.empty()) return;
        }
    }
    fp_phase_ = 0;
    BuildFrameProcessing();
    if (fp_phase_ == 0) fp_snapshot_.clear(); // (the nodes are only needed while building)
}

void MpvBackend::BuildFrameProcessing() {
    // The state may have moved on while the replies were in flight (a busy period, another file): then this snapshot
    // is dropped; FinishVsBusy / the next tick request a fresh one.
    if (!ctx_ || !initialized_ || !file_loaded_ || !event_ || !SyncAllowed() || fp_request_epoch_ != fp_epoch_) return;
    struct SnapshotScope {
        bool& active;
        explicit SnapshotScope(bool& flag) : active(flag) { active = true; }
        ~SnapshotScope() { active = false; }
    } snapshot_scope(fp_snapshot_active_);
    const auto now = std::chrono::steady_clock::now();

    auto number_property = [&](const char* name, const char* key, double fallback = 0.0) {
        NodeHolder holder(api_->free_node_contents);
        const mpv_node* value = ReadNode(name, holder);
        return value ? NodeNumber(MapValue(value, key), fallback) : fallback;
    };
    const int width = static_cast<int>(number_property("video-params", "w"));
    const int height = static_cast<int>(number_property("video-params", "h"));
    double out_width = number_property("video-out-params", "w");
    double out_height = number_property("video-out-params", "h");
    const bool output_params_known = out_width > 0.0 && out_height > 0.0;
    if (!output_params_known) { out_width = width; out_height = height; }
    const double source_fps = PropertyDouble("container-fps", 0.0);
    const double output_fps = PropertyDouble("estimated-vf-fps", source_fps);
    const double display_hz = PropertyDouble("display-fps", 0.0);
    // Delivered rate. estimated-vf-fps is derived from the timestamps of the last filter-output frames (a PTS rate),
    // so it reads 47.95 for any working x2 chain even when the chain cannot keep up. libmpv has no presented-frame or
    // delivered-frame counter (no vo-frame-count; estimated-display-fps is the display refresh estimate), so these are
    // ESTIMATES over one window of >= 5 s (the same window for the panel and the sync-trace line):
    //   delivered_fps = integral(estimated-vf-fps d time-pos) / dt   (frames the chain handed to the VO per wall second;
    //                   valid while time-pos advances with every queued frame, incl. those the VO then drops)
    //   drops_per_s   = delta(frame-drop-count) / dt                 (frames the VO dropped as late)
    //   presented_fps = delivered_fps - drops_per_s
    //   avsync_slope  = delta(avsync) / dt                           (s/s; ~0 when real-time, ~1 - clock_rate/speed if not)
    // Not a GPU or display-present counter. delta(time-pos)/dt already contains `speed`, so delivered_fps is compared
    // with estimated-vf-fps x speed. The window is emptied on every clock discontinuity (ResetRateWindow: seek, pause,
    // speed, vf, VapourSynth busy, file change) and while the clock is frozen; values read null until it spans 5 s.
    const double avsync = PropertyDouble("avsync", 0.0);
    const double drop_count = PropertyDouble("frame-drop-count", -1.0);
    const double position = PropertyDouble("time-pos", 0.0);
    // Sample time = when time-pos was read (its reply), as the synchronous read used to follow `now` directly.
    const FpSnapshotValue* position_read = FpSnapshotFind('D', "time-pos");
    const auto sample_at = position_read && position_read->ok ? position_read->at : now;
    const auto cached_speed = observed_cache_.find("speed"); // observed: no libmpv call
    const double speed = cached_speed != observed_cache_.end() && cached_speed->second.has_number && cached_speed->second.number > 0.0
        ? cached_speed->second.number : 1.0;
    {
        constexpr double kRateWindowS = 5.0;
        if (ObservedFlag("pause") || ObservedFlag("seeking") || ObservedFlag("paused-for-cache")) {
            if (!rate_samples_.empty()) ResetRateWindow(ObservedFlag("pause") ? "pause" : ObservedFlag("seeking") ? "seeking" : "cache");
        } else if (drop_count >= 0.0) {
            RateSample sample{sample_at, position, drop_count, avsync, output_fps, 0.0};
            if (!rate_samples_.empty()) {
                const RateSample& last = rate_samples_.back();
                const double gap = std::chrono::duration<double>(sample_at - last.at).count();
                const double clock_rate = gap > 0.0 ? (position - last.pos) / gap : -1.0;
                // Same 0..4 clock-rate guard as before, plus: counters going backwards, or no tick for > 3 s.
                if (gap > 3.0) ResetRateWindow("gap");
                else if (clock_rate < 0.0 || clock_rate > 4.0 || drop_count < last.drops) ResetRateWindow("jump");
            }
            if (!rate_samples_.empty()) {
                const RateSample& last = rate_samples_.back();
                sample.frames = last.frames + 0.5 * (last.vf_fps + output_fps) * (position - last.pos);
            }
            rate_samples_.push_back(sample);
            // Keep the newest sample that is at least 5 s old as the window start.
            while (rate_samples_.size() >= 2 &&
                   std::chrono::duration<double>(sample_at - rate_samples_[1].at).count() >= kRateWindowS)
                rate_samples_.pop_front();
        }
        const double span = rate_samples_.size() >= 2
            ? std::chrono::duration<double>(rate_samples_.back().at - rate_samples_.front().at).count() : 0.0;
        if (span >= kRateWindowS) {
            const RateSample& first = rate_samples_.front();
            const RateSample& last = rate_samples_.back();
            rate_window_s_ = span;
            rate_delivered_fps_ = std::max(0.0, (last.frames - first.frames) / span);
            rate_drops_delta_ = last.drops - first.drops;
            rate_avsync_slope_ = (last.avsync - first.avsync) / span;
            if (std::abs(rate_avsync_slope_) < 0.0005) rate_avsync_slope_ = 0.0; // prints 0, not -0
            sync_drops_per_s_ = rate_drops_delta_ / span;
            // More drops than delivered frames means the two inputs do not describe the same frames (observed under
            // display-resample with a stalling chain; the derivation was only checked under video-sync=audio): report no
            // estimate rather than 0.
            sync_presented_fps_ = sync_drops_per_s_ > rate_delivered_fps_ + 0.5 ? -1.0
                : std::max(0.0, rate_delivered_fps_ - sync_drops_per_s_);
        } else {
            rate_window_s_ = -1.0;
            rate_delivered_fps_ = -1.0;
            rate_drops_delta_ = -1.0;
            rate_avsync_slope_ = 0.0;
            sync_drops_per_s_ = -1.0;
            sync_presented_fps_ = -1.0;
        }
    }
    // VapourSynth upscale through vs-mlrt (DirectML or TensorRT), as opposed to a GLSL upscaler.
    const bool vs_upscale = round6_upscale_backend_ == "DirectML" || round6_upscale_backend_ == "TensorRT";
    const std::string backend = round6_interpolation_active_ && (round6_interpolation_mode_ == "rife" || round6_interpolation_mode_ == "drba" ||
        round6_interpolation_mode_ == "mvtools") ? "vapoursynth_interpolation" :
        round6_interpolation_active_ && round6_interpolation_mode_ == "amf_frc" ? "amf_frc" :
        round6_upscale_active_ && round6_upscale_backend_ == "DirectML" ? "vapoursynth_dml" :
        round6_upscale_active_ && round6_upscale_backend_ == "TensorRT" ? "vapoursynth_trt" : round6_interpolation_mode_ == "mpv_smooth" ? "mpv_smooth" :
        (!enhancement_plan_.shader_paths.empty() ? "mpv_glsl" : "mpv");

    double gpu_ms = -1.0;
    NodeHolder passes_holder(api_->free_node_contents);
    if (const mpv_node* passes = ReadNode("vo-passes", passes_holder)) {
        const mpv_node* fresh = MapValue(passes, "fresh");
        if (fresh && fresh->format == MPV_FORMAT_NODE_ARRAY && fresh->u.list) {
            gpu_ms = 0.0;
            for (int i = 0; i < fresh->u.list->num; ++i)
                gpu_ms += NodeNumber(MapValue(&fresh->u.list->values[i], "avg")) / 1000000.0;
        }
    }

    int scale = enhancement_plan_.upscale_scale;
    if (scale < 1) scale = 1;
    // VO-side (GLSL) upscale: video-out-params is the size before the VO shaders, so estimate the shader output. This
    // also holds with a VapourSynth interpolation in vf (plan.filter set); a vf upscaler (nv_rtx_vsr) reports its own
    // size in video-out-params and keeps the old path.
    const bool glsl_upscale_active = round6_upscale_active_ && round6_upscale_backend_ == "GLSL";
    if (enhancement_plan_.upscale != "off" && (enhancement_plan_.filter.empty() || glsl_upscale_active)) {
        out_width = std::max(out_width, static_cast<double>(width * scale));
        out_height = std::max(out_height, static_cast<double>(height * scale));
    }
    if (round6_upscale_active_ && vs_upscale && width > 0 && height > 0 && !output_params_known) {
        const int input_height = round6_upscale_pre_height_ > 0 ? std::min(height, round6_upscale_pre_height_) : height;
        const int cap = round6_upscale_max_height_ > 0 ? round6_upscale_max_height_ : std::max(1, MonitorHeight(host_));
        const int generated_height = std::max(1, std::min(input_height * std::max(1, round6_upscale_scale_), cap));
        out_height = generated_height;
        out_width = std::max(1.0, std::round(static_cast<double>(width) * generated_height / height));
    }
    std::string shader_json = "[";
    for (std::size_t i = 0; i < enhancement_plan_.shader_ids.size(); ++i) {
        if (i) shader_json += ',';
        shader_json += Json::Quote(enhancement_plan_.shader_ids[i]);
    }
    shader_json += ']';
    const bool round6_upscale_requested = !round6_upscale_mode_.empty() && round6_upscale_mode_ != "off";
    const bool round6_interpolation_requested = !round6_interpolation_mode_.empty() && round6_interpolation_mode_ != "off";
    const bool any_requested = round6_upscale_requested || round6_interpolation_requested || enhancement_plan_.preset != "custom" || enhancement_plan_.restore != "off" ||
        enhancement_plan_.upscale != "off" || enhancement_plan_.sharpen != "off" ||
        enhancement_plan_.deband != "off" || enhancement_plan_.smooth != "off";
    std::string reason = enhancement_plan_.reason;
    // AMD FRC is a fixed 2x: hint (never block, plan section 12.11) whether 2x source fps maps onto an even number of
    // refreshes. even: display/(2*fps) is an integer within 0.2% (48 on 144/96/48 Hz); uneven: 48 on 120 Hz (2:3);
    // over: the output exceeds the refresh rate. Computed live so a display change updates it without a re-apply.
    std::string amf_cadence;
    if (round6_interpolation_mode_ == "amf_frc" && source_fps > 0.0 && display_hz > 0.0) {
        const double amf_output = 2.0 * source_fps;
        const double ratio = display_hz / amf_output;
        if (amf_output > display_hz * 1.002) amf_cadence = "over";
        else amf_cadence = std::abs(ratio - std::round(ratio)) <= ratio * 0.002 ? "even" : "uneven";
    }
    // Active pipeline in execution order for the info panel: decoder -> vf stages (VS upscale, interpolation,
    // AMD FRC) -> VO shaders -> mpv smoothing. Empty when no processing stage is active.
    const std::string hwdec_current = PropertyString("hwdec-current");
    std::vector<std::string> chain_stages;
    if (round6_upscale_active_ && vs_upscale) {
        std::string stage = "UAI(" + round6_upscale_backend_;
        if (height > 0) {
            const int input_height = round6_upscale_pre_height_ > 0 ? std::min(height, round6_upscale_pre_height_) : height;
            const int cap = round6_upscale_max_height_ > 0 ? round6_upscale_max_height_ : std::max(1, MonitorHeight(host_));
            stage += ", " + std::to_string(input_height) + "→" +
                std::to_string(std::max(1, std::min(input_height * std::max(1, round6_upscale_scale_), cap)));
        }
        chain_stages.push_back(stage + ")");
    }
    if (round6_interpolation_active_ && round6_interpolation_mode_ != "mpv_smooth") {
        const std::string& mode = round6_interpolation_mode_;
        std::string stage = mode == "rife" ? "RIFE" : mode == "drba" ? "DRBA" : mode == "mvtools" ? "MVTools" : "AMD FRC";
        if (mode == "rife" || mode == "drba") stage += "(" + std::string(ai_backend_ == "trt" ? "TensorRT" : "DirectML") + ")";
        stage += " ×" + Number(round6_interpolation_factor_, 3);
        if (mode != "amf_frc" && round6_interpolation_pre_height_ > 0) stage += " H_Pre " + std::to_string(round6_interpolation_pre_height_);
        chain_stages.push_back(stage);
    }
    if (!enhancement_plan_.shader_paths.empty()) {
        const bool glsl_upscale = round6_upscale_backend_ == "GLSL" && !round6_upscale_label_.empty();
        chain_stages.push_back("GLSL(" + (glsl_upscale ? round6_upscale_label_ + ", " : std::string()) +
                               std::to_string(enhancement_plan_.shader_paths.size()) + ")");
    }
    if (enhancement_plan_.deband_active) chain_stages.push_back("deband");
    if (round6_interpolation_mode_ == "mpv_smooth" && round6_interpolation_active_) chain_stages.push_back("mpv smooth");
    std::string chain;
    if (!chain_stages.empty()) {
        // hwdec-current is empty until the first frame is decoded: show "decoder" rather than claiming software.
        chain = hwdec_current.empty() ? std::string("decoder") : hwdec_current == "no" || hwdec_current == "none" ? std::string("software") : hwdec_current;
        for (const auto& stage : chain_stages) chain += " → " + stage;
    }
    // sync-trace: every 5 s while a VapourSynth or amf_frc stage is active, so a log shows whether the A/V offset and
    // the drops grow, oscillate or settle.
    const bool filter_stage_active = (round6_upscale_active_ && vs_upscale) ||
        (round6_interpolation_active_ && round6_interpolation_mode_ != "mpv_smooth");
    // vo-delayed-frame-count is defined only for display-sync (mpv manual); under video-sync=audio it is always 0, so it
    // is printed / sent only for display-* modes (mpv smooth, vsync_auto display-resample).
    const std::string video_sync = PropertyString("video-sync");
    const bool display_sync_mode = video_sync.rfind("display-", 0) == 0;
    const double delayed = display_sync_mode ? PropertyDouble("vo-delayed-frame-count", 0.0) : -1.0;
    const bool rate_valid = rate_window_s_ > 0.0;
    if (filter_stage_active && (sync_trace_at_.time_since_epoch().count() == 0 || now - sync_trace_at_ >= std::chrono::seconds(5))) {
        const double decoder_drops = PropertyDouble("decoder-frame-drop-count", 0.0);
        // Every delta and rate on this line is over the same window (window_s, >= 5 s); n/a after a discontinuity.
        AppendLog("sync-trace pos=" + Number(position, 1) +
                  " window_s=" + (rate_valid ? Number(rate_window_s_, 2) : "n/a(reset:" + rate_reset_reason_ + ")") +
                  " avsync_ms=" + Number(avsync * 1000.0, 1) +
                  " avsync_slope=" + (rate_valid ? Number(rate_avsync_slope_, 3) : std::string("n/a")) +
                  " frame-drop-count=" + Number(std::max(0.0, drop_count), 0) +
                  (rate_valid && rate_drops_delta_ >= 0.0 ? " (+" + Number(rate_drops_delta_, 0) + ")" : std::string()) +
                  " decoder-frame-drop-count=" + Number(decoder_drops, 0) +
                  (display_sync_mode ? " vo-delayed-frame-count=" + Number(delayed, 0) : std::string()) +
                  " video-sync=" + (video_sync.empty() ? std::string("-") : video_sync) + " speed=" + Number(speed, 3) +
                  " estimated-vf-fps=" + Number(output_fps, 3) +
                  " delivered_fps_est=" + (rate_valid ? Number(rate_delivered_fps_, 2) : std::string("n/a")) +
                  " drops_per_s=" + (sync_drops_per_s_ >= 0.0 ? Number(sync_drops_per_s_, 2) : std::string("n/a")) +
                  " presented_fps_est=" + (sync_presented_fps_ >= 0.0 ? Number(sync_presented_fps_, 2) : std::string("n/a")) +
                  " stages=" + (chain.empty() ? std::string("-") : chain));
        sync_trace_at_ = now;
    }
    const std::string json = "{\"engine\":\"mpv\",\"requested\":" + std::string(any_requested ? "true" : "false") +
        ",\"active\":" + std::string(enhancement_plan_.active ? "true" : "false") +
        ",\"state\":\"" + std::string(enhancement_plan_.active ? "active" : "off") + "\",\"backend\":" + Json::Quote(backend) +
        ",\"reason\":" + Json::Quote(!reason.empty() ? reason : !round6_interpolation_reason_.empty() ? round6_interpolation_reason_ : round6_upscale_reason_) +
        ",\"source_fps\":" + Number(source_fps) +
        ",\"output_fps\":" + Number(output_fps) + ",\"display_hz\":" + Number(display_hz) +
        // Delivered-rate figures (see above), all over one >= 5 s window (rate_window_s) and null until it is valid:
        // avsync in ms (positive = video behind audio) and its slope in s/s, VO drops per second, the delivered and the
        // presented rate. delivered_fps / presented_fps are estimates (no libmpv counter); *_kind says so for readers.
        ",\"avsync_ms\":" + Number(avsync * 1000.0, 1) +
        ",\"avsync_slope\":" + (rate_valid ? Number(rate_avsync_slope_, 3) : std::string("null")) +
        ",\"rate_window_s\":" + (rate_valid ? Number(rate_window_s_, 2) : std::string("null")) +
        ",\"rate_window_reset\":" + Json::Quote(rate_valid ? std::string() : rate_reset_reason_) +
        ",\"speed\":" + Number(speed, 3) +
        ",\"delivered_fps\":" + (rate_valid ? Number(rate_delivered_fps_, 2) : std::string("null")) +
        ",\"delivered_fps_kind\":\"estimate\",\"delivered_fps_method\":\"integral(estimated-vf-fps*dpos)/dt\"" +
        ",\"drops_per_s\":" + (sync_drops_per_s_ >= 0.0 ? Number(sync_drops_per_s_, 2) : std::string("null")) +
        ",\"presented_fps\":" + (sync_presented_fps_ >= 0.0 ? Number(sync_presented_fps_, 2) : std::string("null")) +
        ",\"presented_fps_kind\":\"estimate\",\"presented_fps_method\":\"delivered_fps-drops/s\"" +
        ",\"upscale\":{\"mode\":" + Json::Quote(round6_upscale_requested ? round6_upscale_mode_ : enhancement_plan_.upscale) + ",\"preset\":\"custom\"" +
        ",\"active\":" + std::string(round6_upscale_active_ || enhancement_plan_.upscale != "off" ? "true" : "false") +
        ",\"state\":" + Json::Quote(round6_upscale_active_ || enhancement_plan_.upscale != "off" ? "active" : "off") +
        ",\"reason\":" + Json::Quote(round6_upscale_reason_.empty() ? reason : round6_upscale_reason_) + ",\"in_width\":" + std::to_string(width) +
        ",\"in_height\":" + std::to_string(height) + ",\"out_width\":" + std::to_string(static_cast<int>(out_width)) +
        ",\"out_height\":" + std::to_string(static_cast<int>(out_height)) + ",\"model\":" + Json::Quote(vs_upscale ? round6_upscale_model_ : enhancement_plan_.ai_model_file) +
        ",\"pre_height\":" + std::to_string(vs_upscale ? round6_upscale_pre_height_ : enhancement_plan_.ai_pre_height) + ",\"estimated_vf_fps\":" + Number(output_fps) + ",\"shaders\":" + shader_json +
        ",\"gpu_ms\":" + (gpu_ms >= 0 ? Number(gpu_ms) : std::string("null")) + "},\"restore\":{\"requested\":" +
        std::string(enhancement_plan_.restore != "off" ? "true" : "false") + ",\"active\":" +
        std::string(enhancement_plan_.restore != "off" ? "true" : "false") + ",\"model\":" + Json::Quote(enhancement_plan_.restore) +
        "},\"sharpen\":{\"mode\":" + Json::Quote(enhancement_plan_.sharpen) + "},\"deband\":{\"mode\":" +
        Json::Quote(enhancement_plan_.deband) + ",\"active\":" + std::string(enhancement_plan_.deband_active ? "true" : "false") +
        "},\"hdr\":" + HdrStateJson() + ",\"vrr\":{\"requested\":false},\"vs_script\":" + Json::Quote(vs_script_) +
        ",\"vs_upscale\":" + Json::Quote(vs_upscale ? round6_upscale_label_ : std::string()) +
        ",\"vs_interpolation\":" + Json::Quote(round6_interpolation_mode_ == "off" ? std::string() : round6_interpolation_label_) +
        ",\"upscale_label\":" + Json::Quote(round6_upscale_label_) + ",\"upscale_backend\":" + Json::Quote(round6_upscale_backend_) +
        ",\"upscale_scale\":" + std::to_string(round6_upscale_scale_) + ",\"upscale_pre_height\":" + std::to_string(round6_upscale_pre_height_) +
        ",\"upscale_max_height\":" + std::to_string(round6_upscale_max_height_) + ",\"interpolation_mode\":" + Json::Quote(round6_interpolation_mode_) +
        ",\"upscale_active\":" + std::string(round6_upscale_active_ ? "true" : "false") +
        ",\"interpolation_active\":" + std::string(round6_interpolation_active_ ? "true" : "false") +
        ",\"interpolation_label\":" + Json::Quote(round6_interpolation_label_) + ",\"interpolation_factor\":" + Number(round6_interpolation_factor_) +
        ",\"fps_num\":" + std::to_string(round6_interpolation_fps_num_) + ",\"fps_den\":" + std::to_string(round6_interpolation_fps_den_) +
        ",\"interpolation_reason\":" + Json::Quote(round6_interpolation_reason_) + ",\"amf_cadence\":" + Json::Quote(amf_cadence) +
        // VapourSynth stage failures seen in mpv's log for this file (first line of the error), see HandleVapourSynthFailureLine.
        ",\"upscale_reason\":" + Json::Quote(round6_upscale_reason_) +
        // Audit 7.2c: the failed VapourSynth stages were taken out of vf and the configured decoder restored.
        ",\"vs_failed_removed\":" + std::string(vs_dead_removed_ ? "true" : "false") +
        ",\"upscale_error\":" + Json::Quote(round6_upscale_reason_.empty() ? std::string() : vs_upscale_error_) +
        ",\"interpolation_error\":" + Json::Quote(round6_interpolation_reason_ == "vs_interpolation_failed" ||
                                                 round6_interpolation_reason_ == "vs_chain_failed" ? vs_interpolation_error_ : std::string()) +
        // Generated VapourSynth scripts in the vf chain: 1 when upscale + interpolation share one chain script.
        ",\"vs_scripts\":" + std::to_string(!vs_chain_script_.empty() ? 1 : (vs_up_script_.empty() ? 0 : 1) + (vs_fi_script_.empty() ? 0 : 1)) +
        ",\"upscale_pre_auto\":" + std::string(round6_upscale_pre_auto_ ? "true" : "false") +
        ",\"hwdec_current\":" + Json::Quote(hwdec_current) + ",\"chain\":" + Json::Quote(chain) +
        // Resolved VapourSynth inference backend (empty without an AI stage) and why; trt_installed for the settings UI.
        ",\"ai_backend\":" + Json::Quote(ai_backend_) + ",\"ai_backend_reason\":" + Json::Quote(ai_backend_reason_) +
        ",\"trt_installed\":" + std::string(trt_installed_ ? "true" : "false") +
        // Live packet bitrates (bits/s) measured by mpv over the last second; 0 until enough packets were read.
        ",\"video_bitrate\":" + Number(PropertyDouble("video-bitrate", 0.0), 0) +
        ",\"audio_bitrate\":" + Number(PropertyDouble("audio-bitrate", 0.0), 0) +
        // A few of mpv's own stats-page entries (renderer, sync, audio output) for the info panel.
        ",\"render\":{\"vo\":" + Json::Quote(PropertyString("current-vo")) + ",\"gpu_context\":" + Json::Quote(PropertyString("current-gpu-context")) +
        ",\"display\":" + Json::Quote(PropertyString("display-names")) + ",\"display_hz\":" + Number(display_hz, 2) +
        ",\"osd_w\":" + Number(PropertyDouble("osd-width", 0.0), 0) + ",\"osd_h\":" + Number(PropertyDouble("osd-height", 0.0), 0) + "}" +
        // delayed: vo-delayed-frame-count, null outside display-* sync modes (where mpv does not maintain it).
        ",\"sync\":{\"mode\":" + Json::Quote(video_sync) + ",\"dropped\":" + Number(PropertyDouble("frame-drop-count", 0.0), 0) +
        ",\"decoder_dropped\":" + Number(PropertyDouble("decoder-frame-drop-count", 0.0), 0) +
        ",\"delayed\":" + (display_sync_mode ? Number(delayed, 0) : std::string("null")) + "}" +
        ",\"audio_out\":" + AudioOutputJson() + "}";
    Emit("frameProcessing", json);
}

int MpvBackend::Open(const std::string& path, std::int64_t start_ms, bool paused, bool no_audio) {
    if (!ctx_ || !initialized_) return MPV_ERROR_UNINITIALIZED;
    if (path.empty()) { // 2B: main.cpp no longer reports a rejected open; this one is reported here, as before
        EmitError("MPV_OPEN", "libmpv rejected the media open request: " + std::to_string(MPV_ERROR_UNINITIALIZED), true);
        return MPV_ERROR_UNINITIALIZED;
    }
#if defined(VP_FAULT_INJECTION)
    if (g_fault.flood_queue && host_) { // before the async writes below, whose replies are the next wake
        int flooded = 0;
        while (PostMessageW(host_, WM_NULL, 0, 0)) ++flooded;
        AppendLog("fault injection: posted " + std::to_string(flooded) + " WM_NULL until PostMessage failed with " + std::to_string(GetLastError()));
    }
#endif
    if (path != current_path_) device_loss_retries_ = 0;
    device_loss_original_pending_ = false;
    lan_stream_ = path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
    current_path_ = path;
    file_loaded_ = false;
    ended_emitted_ = false;
    pending_open_ = true;
    first_frame_pending_ = false;
    stop_requested_ = false;
    open_requested_at_ = std::chrono::steady_clock::now();
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    // UI writes: queued in order before the loadfile below, results only logged (OnAsyncReply). A previous file's
    // VapourSynth init can still be running on mpv's playback thread; nothing here waits for it.
    SetPropertyAsync("audio-device", current_audio_device_.empty() ? "auto" : current_audio_device_);
    SetPropertyAsync("audio-exclusive", audio_exclusive_);
    SetPropertyAsync("audio-channels", audio_channels_);
    // Bit-perfect coupling: the level the new file starts at (video-sync is decided in on_preloaded).
    SetPropertyAsync("volume", std::to_string(audio_exclusive_ == "yes" ? 100 : audio_volume_));
    SetPropertyAsync("mute", audio_mute_ ? "yes" : "no");
    const std::string start = "start=" + Number(std::max<std::int64_t>(0, start_ms) / 1000.0, 3);
    const std::string pause = std::string("pause=") + (paused ? "yes" : "no");
    AppendLog("open request start_ms=" + std::to_string(std::max<std::int64_t>(0, start_ms)) +
        "; audio=" + (no_audio ? "off" : "on"));
    std::string per_file_options = start + "," + pause;
    if (no_audio) per_file_options += ",aid=no";
    if (!lan_stream_) {
        const std::string fonts_option = LocalSubFontsDirOption(path);
        if (!fonts_option.empty()) AppendLog("per-file subtitle fonts" + fonts_option);
        per_file_options += fonts_option;
    }
    open_start_ms_ = std::max<std::int64_t>(0, start_ms);
#if defined(VP_FAULT_INJECTION)
    if (++g_opens == g_fault.open_bad_option) {
        per_file_options += ",start"; // no value: mpv_command_async rejects the loadfile synchronously (-4)
        AppendLog("fault injection: malformed per-file option appended to open " + std::to_string(g_opens));
    }
#endif
    const char* args[] = {"loadfile", path.c_str(), "replace", "-1", per_file_options.c_str(), nullptr};
    const std::uint64_t id = NextReplyId("loadfile");
    BeginLoadRequest(id, "open"); // 2B
    const int result = api_->command_async(ctx_, id, args);
#if defined(VP_FAULT_INJECTION)
    if (result >= 0 && g_fault.open_sleep_ms > 0) {
        AppendLog("fault injection: UI thread held " + std::to_string(g_fault.open_sleep_ms) + " ms after loadfile gen=" + std::to_string(load_.gen));
        Sleep(static_cast<DWORD>(g_fault.open_sleep_ms));
    }
    if (result >= 0 && !g_fault.chain_open.empty()) {
        const std::string chained = std::move(g_fault.chain_open);
        g_fault.chain_open.clear();
        AppendLog("fault injection: chained " + std::string(chained == "<stop>" ? "stop" : "open") +
                  " submitted before any drain (older load gen=" + std::to_string(load_.gen) + ")");
        if (chained == "<stop>") Stop(); else Open(chained, 0, false, false);
        return result;
    }
#endif
    if (result < 0) {
        pending_replies_.erase(id);
        load_replies_.erase(id);
        // 2B: rejected synchronously (e.g. malformed per-file options): one error, the old file stopped, state "error".
        FailLoad("MPV_OPEN", LoadErrorText(result), result, "submit", true);
    }
    return result;
}

// 2B: one load request per Open() / device-loss reload (see LoadRequest in the header).
void MpvBackend::BeginLoadRequest(std::uint64_t reply_id, const char* origin) {
    load_ = {};
    load_.gen = ++load_gen_;
    load_.origin = origin;
    load_.at = std::chrono::steady_clock::now();
    load_replies_[reply_id] = load_.gen;
    load_error_latched_ = false;
}

void MpvBackend::OnLoadfileReply(std::uint64_t id, int error, const void* data) {
    const auto it = load_replies_.find(id);
    if (it == load_replies_.end()) return;
    const std::uint64_t gen = it->second;
    load_replies_.erase(it);
#if defined(VP_FAULT_INJECTION)
    if (++g_loadfile_replies == g_fault.load_reject && error >= 0 && gen == load_.gen) {
        error = MPV_ERROR_COMMAND;
        AppendLog("fault injection: loadfile reply gen=" + std::to_string(gen) + " treated as rejected (" +
                  MpvErrorText(api_ ? api_->error_string : nullptr, error) + ")");
    }
#endif
    if (gen != load_.gen) {
        AppendLog("loadfile reply gen=" + std::to_string(gen) + " stale (current gen=" + std::to_string(load_.gen) + ")" +
                  (error < 0 ? " error=" + MpvErrorText(api_ ? api_->error_string : nullptr, error) + " (not reported)" : std::string()));
        return;
    }
    if (error < 0) {
        // mpv refused the loadfile: it left the playlist alone, so the previous file is still playing.
        FailLoad("MPV_OPEN_REJECTED", LoadErrorText(error), error, "reply", true);
        return;
    }
    std::int64_t entry = -1;
    const auto* command = static_cast<const mpv_event_command*>(data);
    if (command && command->result.format == MPV_FORMAT_NODE_MAP) {
        const mpv_node* value = MapValue(&command->result, "playlist_entry_id");
        if (value && value->format == MPV_FORMAT_INT64) entry = value->u.int64;
    }
    load_.reply_seen = true;
    load_.entry_id = entry;
    AppendLog("loadfile accepted gen=" + std::to_string(gen) + " origin=" + load_.origin + " entry=" + std::to_string(entry));
}

// 2B: the current request failed (or a playing file failed with no request): exactly one fatal error, the loading state
// cleared, the error latched until the next Open / Stop / reload. Retry = a new Open (never blocked).
void MpvBackend::FailLoad(const char* code, const std::string& message, int mpv_error, const char* why, bool stop_player) {
    const bool device_loss = load_.gen && std::strcmp(load_.origin, "device-loss") == 0;
    const std::string final_code = device_loss ? std::string("GPU_DEVICE_LOST") : std::string(code);
    const std::string final_message = device_loss && std::strcmp(code, "GPU_DEVICE_LOST") != 0
        ? "GPU 裝置中斷後無法重新開啟檔案：" + message : message;
    AppendLog(std::string("load failed gen=") + std::to_string(load_.gen) + " origin=" + (load_.gen ? load_.origin : "-") +
              " entry=" + std::to_string(load_.entry_id) + " why=" + why + " error=" + std::to_string(mpv_error) + " (" +
              MpvErrorText(api_ ? api_->error_string : nullptr, mpv_error) + ")" +
              (stop_player ? " ; stopping the previous file" : "") + (load_error_latched_ ? " ; already reported" : ""));
    pending_open_ = false;
    stop_requested_ = false;
    first_frame_pending_ = false;
    file_loaded_ = false;
    if (device_loss) device_loss_original_pending_ = false;
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    if (video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_HIDE);
    load_ = {};
    current_path_.clear();
    if (stop_player) Command({"stop"}); // its END_FILE(stop) has no request: "stopped" is swallowed by the latch
    if (load_error_latched_) return;
    EmitError(final_code, final_message, true);
    EmitState("error");
    load_error_latched_ = true;
}

// mpv's error string stays in the text (truthful); the common load errors get a Chinese lead-in.
std::string MpvBackend::LoadErrorText(int mpv_error) const {
    const char* lead = mpv_error == MPV_ERROR_LOADING_FAILED ? "無法載入檔案"
        : mpv_error == MPV_ERROR_UNKNOWN_FORMAT ? "無法辨識的檔案格式"
        : mpv_error == MPV_ERROR_NOTHING_TO_PLAY ? "檔案中沒有可播放的影音軌"
        : mpv_error == MPV_ERROR_VO_INIT_FAILED ? "視訊輸出初始化失敗"
        : mpv_error == MPV_ERROR_AO_INIT_FAILED ? "音訊輸出初始化失敗"
        : "無法開啟檔案";
    return std::string(lead) + "（mpv：" + MpvErrorText(api_ ? api_->error_string : nullptr, mpv_error) + "）";
}

bool MpvBackend::ConfigureAudio(const Json& audio_config) {
    if (!ctx_ || !api_ || !initialized_) return false;
    const bool was_bitperfect = audio_exclusive_ == "yes";
    const std::string old_device = current_audio_device_, old_exclusive = audio_exclusive_, old_channels = audio_channels_,
                      old_clock = master_clock_;
    const int old_volume = audio_volume_;
    const bool old_mute = audio_mute_;
    current_audio_device_ = audio_config["device"].str();
    audio_exclusive_ = audio_config["exclusive"].boolean(false) ? "yes" : "no";
    audio_channels_ = audio_config["channels"].str("stereo");
    audio_volume_ = static_cast<int>(std::clamp<std::int64_t>(audio_config["volume"].i64(audio_volume_), 0, 100));
    audio_mute_ = audio_config["mute"].boolean(audio_mute_);
    master_clock_ = NormalizeMasterClock(audio_config["masterClock"].str());
    const bool bitperfect = audio_exclusive_ == "yes";
    if (master_clock_ != old_clock)
        AppendLog("master-clock " + old_clock + " -> " + master_clock_ + (bitperfect ? " (saved; bit-perfect keeps the audio clock)" : ""));
    if (master_clock_ != old_clock && current_audio_device_ == old_device && audio_exclusive_ == old_exclusive &&
        audio_channels_ == old_channels && audio_volume_ == old_volume && audio_mute_ == old_mute) {
        // Master Clock only: video-sync / interpolation are live options. No AO write (device, exclusive, channels,
        // volume, mute untouched), no vf / VapourSynth rebuild. Idle: the next file's on_preloaded pass decides.
        if (!file_loaded_) return true;
        ApplyAudioSyncPolicy("master-clock");
        last_frame_processing_emit_ = {};
        EmitFrameProcessing();
        return true;
    }
    const std::string mute_text = audio_mute_ ? "yes" : "no";
    const std::string level = std::to_string(bitperfect ? 100 : audio_volume_);
    if (!file_loaded_) {
        // Nothing plays: the next Open() sends exclusive / volume / mute, on_preloaded decides video-sync.
        SetPropertyAsync("volume", level);
        SetPropertyAsync("mute", mute_text);
        SetOptionIfChanged("audio-pitch-correction", bitperfect ? "no" : "yes");
        if (was_bitperfect != bitperfect)
            AppendLog(std::string("audio-transition ") + (bitperfect ? "off->on" : "on->off") + " (idle; applied at the next open)");
        return true;
    }
    const bool device_ok = SetPropertyString("audio-device", current_audio_device_.empty() ? "auto" : current_audio_device_);
    const bool channels_ok = SetPropertyString("audio-channels", audio_channels_);
    bool exclusive_ok = true;
    if (was_bitperfect == bitperfect) {
        exclusive_ok = SetPropertyString("audio-exclusive", audio_exclusive_);
        SetPropertyAsync("volume", level);
        SetPropertyAsync("mute", mute_text);
        ApplyAudioSyncPolicy("configure-audio");
    } else if (bitperfect) {
        // OFF -> ON (the UI asked the user first when the saved level is below 100): silence, reopen the AO in exclusive
        // mode (about 12 ms while paused), go to 100 % while still muted, switch to the audio clock, then restore the
        // user's mute. The saved audio.volume is kept for the way back.
        AppendLog("audio-transition off->on step=1 mute=yes (saved volume=" + std::to_string(audio_volume_) + ")");
        SetPropertyString("mute", "yes");
        AppendLog("audio-transition off->on step=2 audio-exclusive=yes (AO reinit)");
        exclusive_ok = SetPropertyString("audio-exclusive", "yes");
        exclusive_requested_at_ = std::chrono::steady_clock::now();
        if (display_sync_used_) {
            // Still muted: re-select the audio track so mpv builds a new audio chain without the leftover resampler
            // (an AO reinit alone keeps it). Audio re-syncs to the video clock; vf / VapourSynth are not touched.
            std::string aid = last_aid_;
            if (aid.empty()) aid = PropertyString("current-tracks/audio/id");
            if (!aid.empty()) {
                AppendLog("audio-transition off->on step=2b aid=no -> aid=" + aid + " (new audio chain: drops the resampler left by display-resample)");
                SetPropertyString("aid", "no");
                SetPropertyString("aid", aid);
                display_sync_used_ = false;
            }
        }
        AppendLog("audio-transition off->on step=3 volume=100");
        SetPropertyString("volume", "100");
        SetOptionIfChanged("audio-pitch-correction", "no");
        const AudioSyncPolicy policy = ApplyAudioSyncPolicy("bitperfect-on");
        AppendLog("audio-transition off->on step=4 video-sync=" + policy.video_sync + " interpolation=" + (policy.interpolation ? "yes" : "no"));
        AppendLog("audio-transition off->on step=5 mute=" + mute_text + " (user state)");
        SetPropertyString("mute", mute_text);
    } else {
        // ON -> OFF: back to the saved level before the shared AO opens, so the level never jumps.
        exclusive_requested_at_ = {};
        AppendLog("audio-transition on->off step=1 volume=" + level + " (saved)");
        SetPropertyString("volume", level);
        AppendLog("audio-transition on->off step=2 audio-exclusive=no (AO reinit)");
        exclusive_ok = SetPropertyString("audio-exclusive", "no");
        SetOptionIfChanged("audio-pitch-correction", "yes");
        const AudioSyncPolicy policy = ApplyAudioSyncPolicy("bitperfect-off");
        AppendLog("audio-transition on->off step=3 video-sync=" + policy.video_sync + " interpolation=" + (policy.interpolation ? "yes" : "no") +
                  " mute=" + mute_text);
        SetPropertyString("mute", mute_text);
    }
    if (!device_ok || !exclusive_ok || !channels_ok)
        AppendLog("audio settings saved; one or more WASAPI options will take effect on the next file open");
    // mpv smooth is unavailable while bit-perfect is on: rebuild the round-6 state (the vf string is unchanged, so
    // SetOptionIfChanged skips it) so frameProcessing reports the mode and its reason.
    if (was_bitperfect != bitperfect && round6_interpolation_mode_ == "mpv_smooth" && !applying_round6_enhancement_) {
        applying_round6_enhancement_ = true;
        ApplyEnhancement(video_config_);
        applying_round6_enhancement_ = false;
    }
    last_frame_processing_emit_ = {};
    EmitFrameProcessing();
    return true;
}

void MpvBackend::SetVolume(int value) {
    if (!ctx_ || !api_ || !initialized_) return;
    audio_volume_ = std::clamp(value, 0, 100);
    if (audio_exclusive_ == "yes") {
        AppendLog("volume " + std::to_string(audio_volume_) + " saved; output stays 100 (bit-perfect)");
        return;
    }
    // Software volume (cubic) after the filter chain: no AO reinit, no vf / VapourSynth rebuild.
    SetPropertyAsync("volume", std::to_string(audio_volume_));
    AppendLog("volume set " + std::to_string(audio_volume_));
}

void MpvBackend::SetMute(bool value) {
    if (!ctx_ || !api_ || !initialized_) return;
    audio_mute_ = value;
    SetPropertyAsync("mute", value ? "yes" : "no");
    AppendLog(std::string("mute set ") + (value ? "yes" : "no"));
}

bool MpvBackend::Command(const std::vector<std::string>& args) {
    if (!ctx_ || !api_ || !initialized_ || args.empty()) return false;
    std::vector<const char*> raw;
    raw.reserve(args.size() + 1);
    for (const auto& arg : args) raw.push_back(arg.c_str());
    raw.push_back(nullptr);
    std::string what = "command";
    for (const auto& arg : args) what += ' ' + arg.substr(0, 120);
    const std::uint64_t id = NextReplyId(what);
    if (api_->command_async(ctx_, id, raw.data()) < 0) {
        pending_replies_.erase(id);
        return false;
    }
    if (vs_busy_) AppendLog("vs-busy async " + what);
    return true;
}

// Transport writes from the UI are asynchronous (the result is only logged): pause / track selection must not wait
// for a VapourSynth init on mpv's playback thread. Toggle reads pause from the observed-property cache while busy.
int MpvBackend::Play() { return SetPropertyAsync("pause", "no") ? 0 : MPV_ERROR_UNINITIALIZED; }
int MpvBackend::Pause() { return SetPropertyAsync("pause", "yes") ? 0 : MPV_ERROR_UNINITIALIZED; }
int MpvBackend::Toggle() { return SetPropertyAsync("pause", PropertyString("pause") == "yes" ? "no" : "yes") ? 0 : MPV_ERROR_UNINITIALIZED; }

// mpv reloads every vapoursynth filter's script on a seek (TensorRT: engine deserialise, seconds): mark the chain busy
// before the command is queued so nothing on the UI thread waits for the reload.
// Every seek passes its flag explicitly; the global hr-seek=yes stays so start= (resume, device-loss reload) is exact.
int MpvBackend::Seek(std::int64_t ms) { return SeekUser(ms, false, true); }
int MpvBackend::SeekRelative(std::int64_t ms) { return SeekUser(ms, true, true); }

void MpvBackend::SetSeekPreference(const std::string& mode) {
    // nopreview (不顯示預覽) only changes what the UI shows while playing; its seeks are exact. The retired "auto"
    // (keyframes while a VapourSynth stage ran) maps to exact, the default.
    const std::string wanted = mode == "keyframes" || mode == "nopreview" ? mode : "exact";
    if (mode == "auto") AppendLog("seek preference legacy auto -> exact (auto was retired)");
    if (wanted == seek_pref_) return;
    if (initialized_) AppendLog("seek preference " + seek_pref_ + " -> " + wanted);
    seek_pref_ = wanted;
}

// playback.seekMode. Measured on S1-01 (1080p HEVC, RX 9060 XT, DirectML), absolute seeks: no AI exact 80 ms vs
// keyframes 22 ms; AJN upscale 1.7 s vs 0.48 s; AJN + RIFE 9.8 s vs 1.7 s (R1 section B.3). keyframes lands up to one
// GOP away (4 s median, 10.4 s max on that file). exact and nopreview both seek exactly.
int MpvBackend::SeekUser(std::int64_t ms, bool relative, bool exact) {
    const bool vs_active = file_loaded_ && VsStageInVf();
    const bool keyframes = !exact && seek_pref_ == "keyframes";
    const std::string mode = keyframes ? "keyframes" : "exact";
    const std::string seconds = relative ? Number(ms / 1000.0, 3) : Number(std::max<std::int64_t>(0, ms) / 1000.0, 3);
    AppendLog("seek mode=" + mode + " pref=" + (exact ? std::string("forced-exact") : seek_pref_) + " vs_active=" +
              (vs_active ? "yes" : "no") + " target=" + (relative ? (ms >= 0 ? "+" : "") + seconds + " relative" : seconds + " absolute"));
    // Every seek command issued while busy is counted: RequestVsBusyEnd compares them with the MPV_EVENT_SEEKs seen, and
    // a seek mpv has not executed yet (still queued, or coalesced into another) makes the end go through the barrier.
    const bool was_busy = vs_busy_;
    const std::string barrier_by = vs_probe_kind_ >= 2 ? vs_barrier_by_ : std::string();
    if (vs_busy_ || vs_active) {
        BeginVsBusy("seek"); // joining cancels a running barrier: it was queued ahead of this command
        ++vs_seek_cmds_;
    }
    const bool sent = Command({"seek", seconds, std::string(relative ? "relative+" : "absolute+") + mode});
    if (!sent && vs_busy_) {
        --vs_seek_cmds_;
        if (!was_busy) EndVsBusy("seek-not-sent");
    }
    // Restart that barrier behind this command: if the seek runs, its MPV_EVENT_SEEK arrives before the barrier's
    // second reply and hands the end to its PLAYBACK_RESTART; if mpv rejects it, the barrier ends the busy period.
    if (sent && vs_busy_ && !barrier_by.empty()) {
        vs_barrier_by_ = barrier_by;
        vs_barrier_since_ = std::chrono::steady_clock::now();
        SendVsBusyProbe(2);
    }
    return sent ? 0 : MPV_ERROR_UNINITIALIZED;
}

int MpvBackend::SelectSubtitle(std::int64_t id) {
    return SetPropertyAsync("sid", id < 0 ? "no" : std::to_string(id)) ? 0 : MPV_ERROR_UNINITIALIZED;
}

int MpvBackend::LoadSubtitle(const std::string& path) {
    return Command({"sub-add", path, "select"}) ? 0 : MPV_ERROR_UNINITIALIZED;
}

int MpvBackend::SelectAudio(std::int64_t id) {
    return SetPropertyAsync("aid", id < 0 ? "no" : std::to_string(id)) ? 0 : MPV_ERROR_UNINITIALIZED;
}

int MpvBackend::Stop() {
    load_ = {};                 // 2B: a stop cancels the load request (its END_FILE, even an error, is not reported)
    load_error_latched_ = false;
    file_loaded_ = false;
    current_path_.clear();
    lan_stream_ = false;
    pending_open_ = false;
    first_frame_pending_ = false;
    stop_requested_ = true;
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    // stop is queued behind a running VapourSynth init (mpv can only act on it once the script load returns); the UI
    // is told so it can say 「等待濾鏡初始化結束…」 instead of looking stuck. END_FILE ends the busy state.
    if (vs_busy_ && !vs_busy_stop_requested_) {
        vs_busy_stop_requested_ = true;
        AppendLog("vs-init stop requested while busy (trigger=" + vs_busy_trigger_ + " elapsed_ms=" +
                  Number(MsSince(vs_busy_since_), 0) + "); stop is queued behind the filter");
        EmitVsInit("start", "stop");
    }
    // The child is not hidden here: the UI covers the video with its curtain and sends
    // releaseSurface after its exit transition, so the ground colour never pops through.
    return Command({"stop"}) ? 0 : MPV_ERROR_UNINITIALIZED;
}

// Resolves video.aiBackend (auto | dml | trt) for the VapourSynth AI stages (UAI upscale, RIFE, DRBA):
//   dml  -> DirectML always.
//   trt  -> TensorRT when the runtime is installed (and the adapter is NVIDIA), else DirectML + a toast.
//   auto -> TensorRT only on an NVIDIA adapter with the runtime installed, else DirectML.
void MpvBackend::ResolveAiBackend(const Json& video_config) {
    std::string configured = Lower(video_config["aiBackend"].str("auto"));
    if (configured != "dml" && configured != "trt") configured = "auto";
    std::string missing;
    trt_installed_ = TensorRtRuntimeInstalled(exe_dir_, &missing);
    const bool nvidia = gpu_vendor_ == "nvidia";
    if (configured == "dml") {
        ai_backend_ = "dml";
        ai_backend_reason_ = "dml_selected";
    } else if (configured == "trt") {
        ai_backend_ = trt_installed_ && nvidia ? "trt" : "dml";
        // Not NVIDIA first: on another vendor installing TensorRT would not help, so name that cause.
        ai_backend_reason_ = !nvidia ? "trt_not_nvidia" : !trt_installed_ ? "trt_not_installed" : "trt_selected";
    } else {
        ai_backend_ = trt_installed_ && nvidia ? "trt" : "dml";
        ai_backend_reason_ = !nvidia ? "auto_not_nvidia" : !trt_installed_ ? "auto_trt_not_installed" : "auto_nvidia_trt";
    }
    AppendLog("ai backend=" + ai_backend_ + " (" + ai_backend_reason_ + ") configured=" + configured + " vendor=" + gpu_vendor_ +
              (trt_installed_ ? std::string(" tensorrt=installed") : " tensorrt=missing:" + missing) +
              (ai_backend_ == "trt" ? " cuda_device=" + std::to_string(cuda_device_id_)
                                    : " dml_device=" + std::to_string(gpu_device_id_)) +
              ((ai_backend_ == "trt" ? cuda_device_id_ : gpu_device_id_) > kK7sfuncMaxGpuIndex
                   ? std::string(" (outside k7sfunc gpu 0-2: AI stages refused, adapter_index_unsupported)") : std::string()));
    if (configured == "trt" && ai_backend_ != "trt" && !trt_fallback_notified_) {
        trt_fallback_notified_ = true;
        EmitError(ai_backend_reason_ == "trt_not_installed" ? "TRT_NOT_INSTALLED" : "TRT_NOT_NVIDIA",
                  ai_backend_reason_ == "trt_not_installed" ? "TensorRT 未安裝，AI 放大與補幀改用 DirectML。"
                                                            : "所選 GPU 不是 NVIDIA，AI 放大與補幀改用 DirectML。", false);
    }
    if (ai_backend_ == "trt") {
        // vsmlrt writes TensorRT engines next to the .onnx file and silently falls back to %TEMP% when it cannot.
        const auto probe = exe_dir_ / L"vs-runtime" / L"vs-plugins" / L"models" / L".vp_trt_write_probe";
        std::error_code ec;
        bool writable = false;
        {
            std::ofstream test(probe, std::ios::binary | std::ios::trunc);
            writable = static_cast<bool>(test);
        }
        std::filesystem::remove(probe, ec);
        if (!writable)
            AppendLog("warning: vs-plugins\\models is not writable; TensorRT engines will be rebuilt under %TEMP% (first use stalls playback)");
    }
}

void MpvBackend::ReleaseSurface() {
    first_frame_pending_ = false;
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    if (video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_HIDE);
}

void MpvBackend::PresentFirstFrame(const char* trigger) {
    if (!first_frame_pending_) return;
    first_frame_pending_ = false;
    pending_open_ = false;
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    if (video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_SHOWNA);
    const double since_open = MsSince(open_requested_at_);
    AppendLog(std::string("first frame presented trigger=") + trigger + " since_open_ms=" + Number(since_open, 1));
    Emit("firstFrame", "{\"trigger\":" + Json::Quote(trigger) + ",\"since_open_ms\":" + Number(since_open, 1) + "}");
}

// Safety net only: if PLAYBACK_RESTART is late (slow VapourSynth start-up), show the child so
// mpv's window gets an expose; firstFrame is still sent when the restart arrives.
void MpvBackend::OnFirstFrameTimeout() {
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    if (!first_frame_pending_) return;
    AppendLog("first frame not presented within " + std::to_string(kFirstFrameSafetyMs) + " ms; showing the surface");
    if (video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_SHOWNA);
}

std::string NormalizeMasterClock(const std::string& value) { return value == "audio" ? "audio" : "auto"; }

AudioSyncPolicy ResolveAudioSyncPolicy(bool bitperfect, const std::string& master_clock, int saved_volume, bool mute,
                                       const std::string& interpolation_mode, double container_fps, double display_fps,
                                       double speed) {
    AudioSyncPolicy policy;
    policy.bitperfect = bitperfect;
    policy.mute = mute;
    policy.volume = bitperfect ? 100 : std::clamp(saved_volume, 0, 100);
    policy.video_sync = "audio";
    policy.interpolation = false;
    policy.requested_clock = NormalizeMasterClock(master_clock);
    policy.effective_clock = "audio";
    if (bitperfect) {
        // display-* resamples the audio (swresample + speed correction, measured): never with bit-perfect, whatever
        // audio.masterClock says (the saved preference is kept for when bit-perfect is turned off).
        policy.reason = interpolation_mode == "mpv_smooth" ? "bitperfect_no_smooth" : "bitperfect";
        return policy;
    }
    if (policy.requested_clock == "audio") {
        // 音訊優先 (Audio, always): video-sync=audio with or without AI upscaling / interpolation. mpv 平滑 needs a
        // display-sync mode, so it cannot blend frames here (reported like the bit-perfect case).
        policy.reason = interpolation_mode == "mpv_smooth" ? "clock_audio_no_smooth" : "clock_audio";
        return policy;
    }
    // Automatic = mpv-lazy's shipped [vsync_auto] profile (profiles.conf:27-32), for every 補幀 mode including the
    // VapourSynth / AMF interpolators (no host override; upstream's condition looks only at the source container_fps,
    // the display fps and speed): audio when container_fps > 32, display_fps / container_fps > 3.2 or speed != 1, else
    // its global display-resample. Upstream's global interpolation=yes is not adopted: interpolation stays the user's
    // 補幀 choice (mpv 平滑), and only where it can work (a display-sync mode), as vsync_auto sets interpolation=no with
    // the audio clock.
    if (std::abs(speed - 1.0) > 1e-9) policy.reason = "speed_not_one";
    else if (container_fps <= 0.0 || display_fps <= 0.0) policy.reason = "fps_unknown";
    else if (container_fps > 32.0) policy.reason = "fps_gt_32";
    else if (display_fps / container_fps > 3.2) policy.reason = "ratio_gt_3.2";
    else {
        policy.video_sync = "display-resample";
        policy.effective_clock = "display";
        policy.interpolation = interpolation_mode == "mpv_smooth";
        policy.reason = "vsync_auto";
    }
    return policy;
}

AudioSyncPolicy MpvBackend::ApplyAudioSyncPolicy(const char* trigger, double source_fps_hint) {
    const bool bitperfect = audio_exclusive_ == "yes";
    double container_fps = source_fps_hint;
    if (container_fps <= 0.0 && file_loaded_) container_fps = PropertyDouble("container-fps", 0.0);
    if (container_fps <= 0.0) container_fps = policy_source_fps_;
    const double display_fps = EffectiveDisplayHz();
    const double speed = PropertyDouble("speed", 1.0);
    const std::string mode = round6_interpolation_mode_.empty() ? std::string("off") : round6_interpolation_mode_;
    const AudioSyncPolicy policy = ResolveAudioSyncPolicy(bitperfect, master_clock_, audio_volume_, audio_mute_, mode, container_fps,
                                                          display_fps, speed);
    // Only these two (live, no reload) are written here; vf / the VapourSynth chain are never touched.
    if (policy.video_sync.rfind("display-", 0) == 0 && (file_loaded_ || preloading_)) display_sync_used_ = true;
    SetOptionIfChanged("video-sync", policy.video_sync);
    SetOptionIfChanged("interpolation", policy.interpolation ? "yes" : "no");
    if (mode == "mpv_smooth") {
        round6_interpolation_active_ = policy.interpolation;
        round6_interpolation_factor_ = policy.interpolation ? 1.0 : 0.0;
        round6_interpolation_reason_ = policy.interpolation ? std::string() : policy.reason;
    }
    const std::string line = "audio-policy bitperfect=" + std::string(bitperfect ? "yes" : "no") + " clock=" + policy.requested_clock +
        " effective_clock=" + policy.effective_clock + " volume=" + std::to_string(policy.volume) + " mute=" + (policy.mute ? "yes" : "no") +
        " video-sync=" + policy.video_sync + " interpolation=" + (policy.interpolation ? "yes" : "no") + " reason=" + policy.reason;
    audio_policy_reason_ = policy.reason;
    if (line != audio_policy_logged_) {
        audio_policy_logged_ = line;
        AppendLog(line + " trigger=" + trigger + " mode=" + mode + " container_fps=" + Number(container_fps, 3) +
                  " display_fps=" + Number(display_fps, 3) + " speed=" + Number(speed, 3) + " saved_volume=" + std::to_string(audio_volume_));
    }
    return policy;
}

namespace {
// Sample-format width and kind for the bit-perfect check: u8 / s16 / s32 / s64 / float / double, planar or not.
int AudioFormatBits(std::string format, bool* is_float) {
    if (!format.empty() && format.back() == 'p') format.pop_back(); // planar: same samples
    *is_float = format == "float" || format == "double";
    if (format == "u8") return 8;
    if (format == "s16") return 16;
    if (format == "s32" || format == "float") return 32;
    if (format == "s64" || format == "double") return 64;
    return 0;
}
} // namespace

// Bit-perfect only when every check passes (R1 A.4); the first failing check is the reason. Called from
// EmitFrameProcessing (SyncAllowed), so the node reads are synchronous.
std::string MpvBackend::BitPerfectReason(bool* ok) {
    *ok = false;
    const std::string ao = PropertyString("current-ao");
    if (ao.empty()) return "no_audio_output";
    if (ao != "wasapi") return "ao_not_wasapi " + ao;
    if (PropertyString("audio-exclusive") != "yes") return "shared_mode";
    std::string in_format, out_format, in_channels, out_channels;
    double in_rate = 0.0, out_rate = 0.0;
    NodeHolder in_holder(api_->free_node_contents);
    if (const mpv_node* node = ReadNode("audio-params", in_holder); node && node->format == MPV_FORMAT_NODE_MAP) {
        in_rate = NodeNumber(MapValue(node, "samplerate"));
        in_format = NodeString(MapValue(node, "format"));
        in_channels = NodeString(MapValue(node, "channels"));
    }
    NodeHolder out_holder(api_->free_node_contents);
    if (const mpv_node* node = ReadNode("audio-out-params", out_holder); node && node->format == MPV_FORMAT_NODE_MAP) {
        out_rate = NodeNumber(MapValue(node, "samplerate"));
        out_format = NodeString(MapValue(node, "format"));
        out_channels = NodeString(MapValue(node, "channels"));
    }
    if (in_rate <= 0.0 || out_rate <= 0.0) return "no_audio_params";
    if (std::llround(in_rate) != std::llround(out_rate))
        return "rate_mismatch " + Number(in_rate, 0) + "\xE2\x86\x92" + Number(out_rate, 0);
    bool in_float = false, out_float = false;
    const int in_bits = AudioFormatBits(in_format, &in_float);
    const int out_bits = AudioFormatBits(out_format, &out_float);
    // Equal, or an integer widening (s16 -> s32, which mpv logs as 32/24 for 24-bit sources): every sample survives.
    const bool format_ok = in_format == out_format ||
        (in_bits > 0 && out_bits > 0 && !in_float && !out_float && out_bits >= in_bits) ||
        (in_bits > 0 && in_bits == out_bits && in_float == out_float);
    if (!format_ok) return "format_mismatch " + in_format + "\xE2\x86\x92" + out_format;
    if (in_channels != out_channels) return "channels_mismatch " + in_channels + "\xE2\x86\x92" + out_channels;
    const double volume = PropertyDouble("volume", 100.0);
    if (std::abs(volume - 100.0) > 1e-6) return "volume " + Number(volume, 0);
    if (PropertyString("mute") == "yes") return "muted";
    const std::string af = PropertyString("af");
    if (!af.empty()) return "af_active " + af;
    const std::string sync = PropertyString("video-sync");
    if (sync.rfind("display-", 0) == 0) return "video_sync " + sync;
    const double correction = PropertyDouble("audio-speed-correction", 1.0);
    if (std::abs(correction - 1.0) > 1e-6) return "speed_correction " + Number(correction, 5);
    // Not visible in any property: after display-resample mpv keeps a resampler in this audio chain.
    if (display_sync_used_) return "resampler_in_chain";
    *ok = true;
    return "";
}

std::string MpvBackend::AudioResamplerState() {
    if (PropertyString("current-ao").empty()) return "no_audio_output";
    std::string in_format, out_format;
    double in_rate = 0.0, out_rate = 0.0;
    const auto read_params = [this](const char* property, double* rate, std::string* format) {
        NodeHolder holder(api_->free_node_contents);
        const mpv_node* node = ReadNode(property, holder);
        if (node && node->format == MPV_FORMAT_NODE_MAP) {
            *rate = NodeNumber(MapValue(node, "samplerate"));
            *format = NodeString(MapValue(node, "format"));
        }
    };
    read_params("audio-params", &in_rate, &in_format);
    read_params("audio-out-params", &out_rate, &out_format);
    if (in_rate <= 0.0 || out_rate <= 0.0) return "unknown";
    std::string state;
    const auto add = [&state](const char* part) { state += (state.empty() ? "" : "+") + std::string(part); };
    bool in_float = false, out_float = false;
    const int in_bits = AudioFormatBits(in_format, &in_float), out_bits = AudioFormatBits(out_format, &out_float);
    const bool lossless_widening = in_bits > 0 && out_bits >= in_bits && !in_float && !out_float;
    if (in_format != out_format && !lossless_widening) add("format");
    if (std::llround(in_rate) != std::llround(out_rate)) add("rate");
    if (std::abs(PropertyDouble("audio-speed-correction", 1.0) - 1.0) > 1e-6) add("speed");
    else if (display_sync_used_) add("leftover");
    return state.empty() ? "none" : state;
}

double MpvBackend::EffectiveDisplayHz() {
    const double reported = PropertyDouble("display-fps", 0.0);
    return reported > 0.0 ? reported : DisplayRefreshHz(host_);
}

bool MpvBackend::SetOptionIfChanged(const char* name, const std::string& value) {
    const auto it = applied_options_.find(name);
    if (it != applied_options_.end() && it->second == value) return true;
    if (!SetPropertyString(name, value)) {
        applied_options_.erase(name);
        return false;
    }
    applied_options_[name] = value;
    return true;
}

void MpvBackend::ApplyHdrOptions(const char* trigger, const std::string& source, const std::string& primaries) {
    // video.hdrOutput -> target-colorspace-hint (set on every pass, from the setting only, never from the source):
    //   auto (default) = mpv's own default: HDR passthrough when the Windows desktop / swapchain is HDR, tone-mapped
    //                    SDR otherwise;
    //   off            = no: always tone-map to SDR. mpv-lazy ships this globally (portable_config/mpv.conf:41);
    //   on             = yes; hint-mode=source (classic passthrough of the source's colour space) only while the
    //                    display under the window has Windows HDR on, else hint-mode=target. Measured on this SDR
    //                    desktop: yes + source makes libplacebo switch the swapchain to RGB_FULL_G2084_NONE_P2020 and
    //                    render PQ (video-target-params gamma=pq) into a desktop Windows composes as SDR -- neither
    //                    passthrough nor mpv's tone mapping; yes + target keeps RGB_FULL_G22_NONE_P709 and tone-maps.
    //                    The panel says that the output is still SDR. HdrStateJson re-applies on a display change.
    // This never changes the Windows HDR setting.
    RefreshDisplayCaps();
    const std::string& setting = hdr_output_setting_;
    const std::string hint = setting == "off" ? "no" : setting == "on" ? "yes" : "auto";
    const std::string hint_mode = setting == "on" && display_caps_.hdr_enabled ? "source" : "target";
    // HDR-source values from mpv-lazy's [HDR_generic] profile (profiles.conf:49-62). Upstream restores them with
    // profile-restore=copy; every non-HDR file (and on_preloaded, where the source is not known yet) gets mpv's
    // defaults back: hdr-reference-white=auto, hdr-peak-percentile=0 (= libplacebo's default), per
    // `mpv --list-options` of this build. They used to stay set for every file after the first HDR one.
    const bool hdr = IsHdrSourceClass(source);
    std::string changed;
    const auto set = [&](const char* name, const std::string& value) {
        const auto it = applied_options_.find(name);
        const bool differs = it == applied_options_.end() || it->second != value;
        const bool ok = SetOptionIfChanged(name, value);
        if (!ok || differs) changed += (changed.empty() ? "" : ",") + std::string(name) + "=" + (ok ? value : value + "(failed)");
    };
    set("target-colorspace-hint", hint);
    set("target-colorspace-hint-mode", hint_mode);
    set("hdr-reference-white", hdr ? "100" : "auto");
    set("hdr-peak-percentile", hdr ? "99.99" : "0");
    // blend-subtitles=yes rendered subtitles into the video frame before scaling/shaders/interpolation: blurrier text
    // under upscaling, and it made sub-ass-force-margins ineffective. Output-resolution rendering is mpv's default and
    // what mpv-lazy uses (commented out in its mpv.conf; [HDR_generic] sets no).
    set("blend-subtitles", "no");
    hdr_source_ = source;
    hdr_source_primaries_ = primaries;
    AppendLog(std::string("hdr-options trigger=") + trigger + " setting=" + setting + " source=" + source +
              (primaries.empty() ? std::string() : "/" + primaries) + " target-colorspace-hint=" + hint +
              " hint-mode=" + hint_mode + " windows_hdr=" + (display_caps_.known ? (display_caps_.hdr_enabled ? "on" : "off") : "unknown") +
              " hdr-reference-white=" + (hdr ? "100" : "auto(default)") +
              " hdr-peak-percentile=" + (hdr ? "99.99" : "0(default)") + " changed=" + (changed.empty() ? "none" : changed));
}

void MpvBackend::RefreshDisplayCaps() {
    const HMONITOR monitor = host_ ? MonitorFromWindow(host_, MONITOR_DEFAULTTONEAREST) : nullptr;
    const auto now = std::chrono::steady_clock::now();
    // A DXGI factory + output walk and one DisplayConfig query: about a millisecond, so at most every 5 s unless the
    // window changed monitor or main.cpp reported WM_DISPLAYCHANGE / WM_MOVE.
    if (!display_caps_dirty_ && monitor == display_caps_.monitor && display_caps_at_.time_since_epoch().count() != 0 &&
        now - display_caps_at_ < std::chrono::seconds(5)) return;
    display_caps_dirty_ = false;
    display_caps_at_ = now;
    const DisplayHdrQuery query = QueryDisplayHdr(monitor);
    display_caps_.monitor = monitor;
    display_caps_.known = query.known;
    display_caps_.hdr_enabled = query.hdr_enabled;
    display_caps_.capable_known = query.capable_known;
    display_caps_.hdr_capable = query.hdr_capable;
    display_caps_.max_nits = query.max_nits;
    display_caps_.device = query.device;
    display_caps_.colorspace = query.colorspace;
    const std::string line = "display-hdr device=" + (query.device.empty() ? std::string("?") : query.device) +
        " colorspace=" + (query.known ? query.colorspace : std::string("unknown")) +
        " windows_hdr=" + (query.known ? (query.hdr_enabled ? "on" : "off") : "unknown") +
        " hdr_capable=" + (query.capable_known ? (query.hdr_capable ? "yes" : "no") : "unknown") +
        " max_nits=" + Number(query.max_nits, 0);
    if (line != display_caps_logged_) {
        display_caps_logged_ = line;
        AppendLog(line);
    }
}

std::string MpvBackend::HdrStateJson() {
    std::string source = hdr_source_, primaries = hdr_source_primaries_;
    double sig_peak = 0.0;
    NodeHolder params_holder(api_->free_node_contents);
    if (const mpv_node* params = ReadNode("video-params", params_holder)) {
        sig_peak = NodeNumber(MapValue(params, "sig-peak"));
        const std::string live = HdrSourceClass(NodeString(MapValue(params, "gamma")), sig_peak);
        if (live != "unknown") {
            source = live;
            primaries = NodeString(MapValue(params, "primaries"));
        }
    }
    // What the VO outputs (libplacebo's target): gamma pq = native HDR output; anything else is SDR.
    std::string target_gamma, target_primaries;
    double target_peak = 0.0;
    NodeHolder target_holder(api_->free_node_contents);
    // Until the first frame of this file is shown, video-target-params still describes the previous file's output.
    if (const mpv_node* target = first_frame_pending_ ? nullptr : ReadNode("video-target-params", target_holder)) {
        target_gamma = NodeString(MapValue(target, "gamma"));
        target_primaries = NodeString(MapValue(target, "primaries"));
        target_peak = NodeNumber(MapValue(target, "max-luma"));
    }
    RefreshDisplayCaps();
    // "on" follows the display (hint-mode source only with Windows HDR on): re-apply after a display / HDR change.
    if (file_loaded_ && hdr_output_setting_ == "on") {
        const auto mode = applied_options_.find("target-colorspace-hint-mode");
        const std::string wanted = display_caps_.hdr_enabled ? "source" : "target";
        if (mode != applied_options_.end() && mode->second != wanted) ApplyHdrOptions("display-change", hdr_source_, hdr_source_primaries_);
    }
    const bool hdr = IsHdrSourceClass(source);
    const bool target_pq = Lower(target_gamma) == "pq";
    // native-hdr only when mpv renders PQ *and* Windows composes the display in HDR. PQ into an SDR desktop
    // (pq-sdr-desktop) is converted by Windows, not shown as HDR; tone-mapped SDR is never called HDR.
    const std::string output = target_gamma.empty() ? std::string("pending")
        : target_pq ? (!display_caps_.hdr_enabled ? "pq-sdr-desktop" : hdr ? "native-hdr" : "sdr-on-hdr")
        : hdr ? "sdr-tonemapped" : "sdr";
    const std::string hint = hdr_output_setting_ == "off" ? "no" : hdr_output_setting_ == "on" ? "yes" : "auto";
    // HDR + AI, upstream parity (no conversion added): k7sfunc decodes and re-encodes the YUV with the BT.709 matrix
    // (mod_mix.py / mod_memc.py), so a PQ/HLG source reaches the model as if it were SDR BT.709; mpv restores the
    // output tags from _MP_IMAGE, so the HDR metadata survives (R1 C.3, measured).
    const bool ai_parity = hdr && VsStageInVf();
    if (ai_parity && !hdr_ai_note_logged_) {
        hdr_ai_note_logged_ = true;
        AppendLog("hdr-ai-parity source=" + source + (primaries.empty() ? std::string() : "/" + primaries) + " vs=" + vs_script_ +
                  ": AI models process PQ/HLG data with the BT.709 matrix (same as mpv-lazy); colour correctness unverified");
    }
    std::vector<std::string> notes;
    if (output == "pq-sdr-desktop") notes.push_back("PQ 訊號送往 SDR 桌面，由 Windows 轉換，並非 HDR 顯示");
    if (hdr && !target_pq && !target_gamma.empty()) {
        if (hdr_output_setting_ == "off")
            notes.push_back(display_caps_.hdr_enabled ? "設定為關閉：Windows HDR 開啟時仍色調映射為 SDR" : "設定為關閉：色調映射為 SDR");
        else if (display_caps_.known && !display_caps_.hdr_enabled)
            notes.push_back(hdr_output_setting_ == "on" ? "已要求 HDR 直通，但 Windows HDR 未開啟：仍色調映射為 SDR"
                                                        : "Windows HDR 未開啟：色調映射為 SDR");
    }
    if (ai_parity) notes.push_back("AI 模型以 BT.709 矩陣處理 PQ/HLG 資料（與 mpv-lazy 相同），色彩正確性未驗證");
    std::string note;
    for (const auto& item : notes) note += (note.empty() ? "" : "；") + item;
    return "{\"active\":" + std::string(hdr ? "true" : "false") + ",\"setting\":" + Json::Quote(hdr_output_setting_) +
        ",\"hint\":" + Json::Quote(hint) + ",\"source\":" + Json::Quote(source) + ",\"primaries\":" + Json::Quote(primaries) +
        ",\"sig_peak\":" + Number(sig_peak, 3) +
        ",\"display\":" + Json::Quote(display_caps_.device) + ",\"display_known\":" + std::string(display_caps_.known ? "true" : "false") +
        ",\"display_hdr_enabled\":" + std::string(display_caps_.hdr_enabled ? "true" : "false") +
        ",\"display_hdr_capable\":" + std::string(!display_caps_.capable_known ? "null" : display_caps_.hdr_capable ? "true" : "false") +
        ",\"display_max_nits\":" + Number(display_caps_.max_nits, 0) + ",\"display_colorspace\":" + Json::Quote(display_caps_.colorspace) +
        ",\"target_gamma\":" + Json::Quote(target_gamma) + ",\"target_primaries\":" + Json::Quote(target_primaries) +
        ",\"target_peak\":" + Number(target_peak, 1) + ",\"output\":" + Json::Quote(output) +
        ",\"ai_bt709_parity\":" + std::string(ai_parity ? "true" : "false") + ",\"note\":" + Json::Quote(note) + "}";
}

void MpvBackend::ApplyFileEnhancement(bool preload) {
    if (!enhancement_configured_) return;
    // FILE_LOADED pass while the chain built in on_preloaded initialises: run it once the chain is up (EndVsBusy).
    if (!preload && !SyncAllowed()) {
        vs_deferred_file_enhancement_ = true;
        AppendLog("enhancement FILE_LOADED pass deferred until the VapourSynth init finishes");
        return;
    }
    if (!preload && !hooks_available_) applied_options_.clear(); // old libmpv: behave as before
    const Json requested_config = video_config_;
    const bool apply_original = lan_stream_ || device_loss_original_pending_;
    if (!preload) device_loss_original_pending_ = false;
    // The original-quality override (LAN stream, device-loss retry) keeps the user's HDR output setting.
    const Json actual_config = apply_original
        ? Json::Parse("{\"preset\":\"original\",\"gpuApi\":\"d3d11\",\"restore\":\"off\",\"upscaler\":\"off\",\"sharpen\":\"off\",\"deband\":\"off\",\"smooth\":\"off\",\"tscale\":\"oversample\",\"hdrOutput\":" +
                      Json::Quote(requested_config["hdrOutput"].str("auto")) + "}")
        : requested_config;
    preloading_ = preload;
    const bool applied = ApplyEnhancement(actual_config);
    preloading_ = false;
    if (!applied) EmitError("ENHANCEMENT_REJECTED", "Could not apply the MPV enhancement settings.", false);
    video_config_ = requested_config;
}

bool MpvBackend::WriteVsScript(const char* prefix, const std::string& signature, const std::string& script,
                               std::string* filter, std::string* error) {
    std::error_code ec;
    const auto scripts_dir = data_dir_ / L"cache" / L"vs";
    std::filesystem::create_directories(scripts_dir, ec);
    if (ec || !filter) {
        if (error) *error = "Could not create the isolated VapourSynth script cache.";
        return false;
    }
    if (!vs_cache_checked_) {
        // Once per process: drop generated scripts from an older schema (separate up_/fi_ files without sections).
        // A handful of small files, read only for their first line.
        vs_cache_checked_ = true;
        int removed = 0;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(scripts_dir)) {
                const std::wstring name = entry.path().filename().wstring();
                if (entry.path().extension() != L".vpy" ||
                    (name.rfind(L"up_", 0) != 0 && name.rfind(L"fi_", 0) != 0 && name.rfind(L"chain_", 0) != 0)) continue;
                std::string first;
                {
                    std::ifstream file(entry.path(), std::ios::binary);
                    std::getline(file, first);
                }
                if (!first.empty() && first.back() == '\r') first.pop_back();
                if (first == kVsScriptSchemaLine) continue;
                std::error_code remove_ec;
                if (std::filesystem::remove(entry.path(), remove_ec)) ++removed;
            }
        } catch (const std::exception&) {
        }
        if (removed) AppendLog("vs-cache removed " + std::to_string(removed) + " stale generated script(s) (before schema 2)");
    }
    std::wostringstream name;
    name << std::wstring(prefix, prefix + std::strlen(prefix)) << L"_" << std::hex
         << StableHash64(std::string(kVsScriptSchemaLine) + "|" + signature) << L".vpy";
    const auto script_path = scripts_dir / name.str();
    if (!WriteFileIfDifferent(script_path, std::string(kVsScriptSchemaLine) + "\n" + script)) {
        if (error) *error = "Could not write the generated VapourSynth script into the isolated cache.";
        return false;
    }
    std::string script_path_utf8 = Utf8(script_path.wstring());
    std::replace(script_path_utf8.begin(), script_path_utf8.end(), '\\', '/');
    // mpv defaults (buffered-frames=4, concurrent-frames=auto), exactly like mpv-lazy's `vf set vapoursynth=...`.
    // Forcing concurrent-frames=2 starved RIFE: 24->48 fps went from 0 to 250-390 drops per 15 s (RX 9060 XT).
    *filter = "vapoursynth=file=%" + std::to_string(script_path_utf8.size()) + "%" + script_path_utf8;
    return true;
}

bool MpvBackend::PrepareUpscaleScript(const EnhancementPlan& plan, VsStageScript* out, std::string* error) {
    const bool trt = ai_backend_ == "trt";
    const char* backend_name = trt ? "TensorRT" : "DirectML";
    if (!out || !plan.ai_dml || plan.ai_model_file.empty()) {
        if (error) *error = std::string("The ") + backend_name + " enhancement plan has no model.";
        return false;
    }
    const auto runtime_root = exe_dir_ / L"vs-runtime";
    const auto template_path = exe_dir_ / L"mpv" / L"templates" / L"vs" / (trt ? L"uai_trt.vpy" : L"uai_dml.vpy");
    const auto model_path = runtime_root / L"vs-plugins" / L"models" / std::filesystem::path(plan.ai_model_file);
    const bool inference_runtime = trt ? TensorRtRuntimeInstalled(exe_dir_)
        : std::filesystem::is_regular_file(runtime_root / L"vs-plugins" / L"vsort.dll") &&
          std::filesystem::is_regular_file(runtime_root / L"vs-plugins" / L"vsort" / L"DirectML.dll");
    if (!std::filesystem::is_regular_file(runtime_root / L"portable.vs") ||
        !std::filesystem::is_regular_file(runtime_root / L"VSScript.dll") || !inference_runtime ||
        !std::filesystem::is_regular_file(model_path) || !std::filesystem::is_regular_file(template_path)) {
        if (error) *error = std::string("The local VapourSynth/") + backend_name + " runtime or selected model is incomplete.";
        return false;
    }

    std::ifstream template_file(template_path, std::ios::binary);
    std::string script((std::istreambuf_iterator<char>(template_file)), std::istreambuf_iterator<char>());
    if (script.empty()) {
        if (error) *error = std::string("The ") + backend_name + " VapourSynth template is empty.";
        return false;
    }
    script = ReplaceAll(std::move(script), "\r\n", "\n");
    // k7sfunc types gpu as Literal[0, 1, 2] and raises otherwise. Never clamped or remapped to another GPU (audit 7.6d);
    // ApplyEnhancement refuses the stage before this (adapter_index_unsupported), this only guards other callers.
    const std::uint32_t device = trt ? cuda_device_id_ : gpu_device_id_;
    if (device > kK7sfuncMaxGpuIndex) {
        if (error) *error = std::string("GPU index ") + std::to_string(device) + " is outside k7sfunc's gpu range 0-2.";
        return false;
    }
    const int max_height = plan.ai_max_height > 0 ? plan.ai_max_height : MonitorHeight(host_);
    script = ReplaceAll(std::move(script), "__H_MAX__", std::to_string(std::max(1, max_height)));
    script = ReplaceAll(std::move(script), "__H_PRE__", std::to_string(plan.ai_pre_height));
    script = ReplaceAll(std::move(script), "__MODEL__", Json::Quote(plan.ai_model_file));
    script = ReplaceAll(std::move(script), "__FP16__", plan.ai_fp16 ? "True" : "False");
    script = ReplaceAll(std::move(script), "__GPU__", std::to_string(device));
    script = ReplaceAll(std::move(script), "__THREADS__", std::to_string(std::clamp(plan.ai_gpu_threads, 1, 3)));
    if (trt) script = ReplaceAll(std::move(script), "__TRT_LOG__", TrtLogLiteral(data_dir_));
    if (script.find("__") != std::string::npos) {
        if (error) *error = std::string("The ") + backend_name + " VapourSynth template has an unresolved setting.";
        return false;
    }
    out->script = std::move(script);
    out->signature = ai_backend_ + ":" + plan.ai_model_file + ":" + std::to_string(plan.ai_pre_height) + ":" +
        std::to_string(max_height) + ":" + std::to_string(device) + ":" + std::to_string(plan.ai_gpu_threads) +
        ":" + (plan.ai_fp16 ? "1" : "0");
    out->template_name = trt ? "uai_trt.vpy" : "uai_dml.vpy";
    out->backend = trt ? "trt" : "dml";
    return true;
}

bool MpvBackend::PrepareInterpolationScript(const Json& settings, const std::string& mode,
                                            double source_fps, double display_fps,
                                            VsStageScript* out, std::string* error,
                                            double* chosen_factor, int* fps_num, int* fps_den,
                                            bool after_vs_upscale, int display_cap_height,
                                            int* resolved_pre_height, bool* pre_height_auto) {
    if (!out || (mode != "mvtools" && mode != "rife" && mode != "drba")) {
        if (error) *error = "Unknown VapourSynth interpolation mode.";
        return false;
    }
    const auto runtime_root = exe_dir_ / L"vs-runtime";
    const auto model_root = runtime_root / L"vs-plugins" / L"models";
    const auto base_plugins = runtime_root / L"vs-plugins";
    // MVTools is CPU-only; RIFE and DRBA run on the resolved AI backend.
    const bool trt = mode != "mvtools" && ai_backend_ == "trt";
    if (!std::filesystem::is_regular_file(runtime_root / L"portable.vs") ||
        !std::filesystem::is_regular_file(runtime_root / L"VSScript.dll")) {
        if (error) *error = "The local VapourSynth runtime is incomplete.";
        return false;
    }
    if (trt ? !TensorRtRuntimeInstalled(exe_dir_)
            : (!std::filesystem::is_regular_file(base_plugins / L"vsort.dll") ||
               !std::filesystem::is_regular_file(base_plugins / L"vsort" / L"DirectML.dll"))) {
        if (error) *error = trt ? "The local TensorRT runtime (vstrt.dll + vsmlrt-cuda) is incomplete."
                                : "The local VapourSynth/DirectML runtime is incomplete.";
        return false;
    }

    const auto factor = ParseFpsFactor(settings["factor"].str("auto"), source_fps, display_fps);
    if (chosen_factor) *chosen_factor = factor.value;
    if (fps_num) *fps_num = factor.num;
    if (fps_den) *fps_den = factor.den;
    // interpolation.preHeight: "auto" (default) or an explicit 0/720/1080/1440 (FMT_CTRL h_max, which only shrinks).
    // auto follows mpv_PlayKit's order (upscale -> shrink to display size -> interpolate, Discussion #313):
    // after a VapourSynth upscaler H_PRE is 0, since the upscaler already caps its output to H_MAX and shrinking
    // again would throw the upscale away; without one it is the same display cap the upscaler would use
    // (upscaleParams.maxHeight, else the monitor height), so a 4K source on a 1440p display is interpolated at 1440.
    // A numeric 1080 without preHeightVersion >= 2 is migrated to auto: it was the implicit default before auto
    // existed, never a user choice. The settings UI writes preHeightVersion 2, so a 1080 chosen there is kept.
    const Json& pre_setting = settings["preHeight"];
    bool pre_auto = true;
    int pre_height = 0;
    if (pre_setting.type() == Json::Type::Number) {
        const int value = static_cast<int>(pre_setting.i64(0));
        const bool legacy_default = value == 1080 && settings["preHeightVersion"].i64(0) < 2;
        if (!legacy_default && (value == 0 || value == 720 || value == 1080 || value == 1440)) {
            pre_auto = false;
            pre_height = value;
        }
    }
    if (pre_auto) pre_height = after_vs_upscale ? 0 : std::max(1, display_cap_height);
    if (resolved_pre_height) *resolved_pre_height = pre_height;
    if (pre_height_auto) *pre_height_auto = pre_auto;
    const int threads = std::clamp(static_cast<int>(settings["gpuThreads"].i64(2)), 1, 3);
    // DRBA scene detection (audit 7.5a): 0 = none (k7sfunc DRBA_DML/DRBA_NV and mpv_PlayKit default; the model also
    // synthesises the frame at a cut), 1 = SCDetect2 luma difference, 2 = MVTools SCDetection; with 1 or 2 a detected cut
    // repeats the pre-cut frame (vsmlrt DRBA selects img1 on _SceneChangeNext). New configs store 0 with scModeVersion 2
    // (main.cpp); a config WITHOUT the key keeps this edition's earlier behaviour, 1, and stored values are used as is.
    const int scene_mode = std::clamp(static_cast<int>(settings["scMode"].i64(1)), 0, 2);
    const bool recal = settings["mvRecal"].boolean(true);
    const bool block = settings["mvBlock"].boolean(true);
    int rife_scene_mode = std::clamp(static_cast<int>(settings["rifeScMode"].i64(1)), 0, 2);
    const std::string rife_id = settings["rifeModel"].str("46");
    std::string model_file;
    int rife_model = 46;
    if (rife_id == "4251") { model_file = "rife_v4.25_lite.onnx"; rife_model = 4251; }
    else if (rife_id == "426") { model_file = "rife_v4.26.onnx"; rife_model = 426; }
    else if (rife_id == "4262") { model_file = "rife_v4.26_heavy.onnx"; rife_model = 4262; }
    else { model_file = "rife_v4.6.onnx"; rife_model = 46; }
    const bool rife_quality = settings["rifeMode"].str("fast") == "quality";
    const int drba_model = std::clamp(static_cast<int>(settings["drbaModel"].i64(2)), 1, 2);
    // TensorRT DRBA has its own setting, drbaTurboTrt: 1 (default, mpv_PlayKit's MEMC_DRBA_NV: static engine, flow at full
    // resolution, drba/<model>_ap.onnx) or 2 (flow at half resolution, drba/<model>_scale_ap.onnx; DRBA_NV supports it,
    // k7sfunc mod_memc.py DRBA_HUB). Turbo 2 needs that export; without it Turbo 1 is used. The DML setting is separate.
    int drba_turbo = trt ? (settings["drbaTurboTrt"].i64(1) == 2 ? 2 : 1) : std::clamp(static_cast<int>(settings["drbaTurbo"].i64(2)), 0, 2);
    if (trt && mode == "drba" && drba_turbo == 2 &&
        !std::filesystem::is_regular_file(model_root / L"drba" /
            (drba_model == 1 ? L"distilDRBA_v1_scale_ap.onnx" : L"distilDRBA_v2_lite_scale_ap.onnx"))) {
        AppendLog("r6-interpolation drba TensorRT Turbo 2 export missing (drba/*_scale_ap.onnx); using Turbo 1");
        drba_turbo = 1;
    }

    std::filesystem::path template_path;
    std::string script;
    if (mode == "mvtools") {
        if (!std::filesystem::exists(base_plugins / L"mvtools.dll")) {
            if (error) *error = "MVTools is not present in the local VapourSynth runtime.";
            return false;
        }
        template_path = exe_dir_ / L"mpv" / L"templates" / L"vs" / L"fi_mvtools.vpy";
    } else if (mode == "rife") {
        // k7sfunc RIFE_ORT_HUB loads exactly one file: quality v4.6 uses only rife\rife_v4.6_ensemble.onnx
        // (s_tta); the other quality models use rife\<name>.onnx; fast mode uses rife_v2\<name>.onnx.
        // RIFE_NV resolves the same files (turbo=0 -> rife\ + ensemble for v4.6, turbo=2 -> rife_v2\).
        const std::filesystem::path required_model = rife_quality
            ? model_root / L"rife" / (rife_model == 46 ? std::filesystem::path(L"rife_v4.6_ensemble.onnx") : std::filesystem::path(model_file))
            : model_root / L"rife_v2" / std::filesystem::path(model_file);
        if (!std::filesystem::is_regular_file(required_model)) {
            if (error) *error = "The selected RIFE model export is not installed.";
            return false;
        }
        if (rife_scene_mode == 2 && !std::filesystem::exists(base_plugins / L"mvtools.dll")) {
            if (error) *error = "RIFE MVTools scene detection requires the local MVTools plugin.";
            return false;
        }
        template_path = exe_dir_ / L"mpv" / L"templates" / L"vs" / (trt ? L"fi_rife_trt.vpy" : L"fi_rife.vpy");
    } else {
        std::string drba_file = drba_model == 1 ? "distilDRBA_v1" : "distilDRBA_v2_lite";
        if (drba_turbo == 2) drba_file += "_scale";
        if (drba_turbo > 0) drba_file += "_ap";
        // DRBA_HUB appends _fp16 only for the DML backend (model_half); the TRT exports have no suffix.
        drba_file += trt ? ".onnx" : "_fp16.onnx";
        if (!std::filesystem::is_regular_file(model_root / L"drba" / std::filesystem::path(drba_file))) {
            if (error) *error = "The selected DRBA model/Turbo export is not installed: " + drba_file;
            return false;
        }
        if (scene_mode == 2 && !std::filesystem::is_regular_file(base_plugins / L"mvtools.dll")) {
            if (error) *error = "DRBA MVTools scene detection requires the local MVTools plugin.";
            return false;
        }
        template_path = exe_dir_ / L"mpv" / L"templates" / L"vs" / (trt ? L"fi_drba_trt.vpy" : L"fi_drba.vpy");
    }

    std::ifstream template_file(template_path, std::ios::binary);
    script.assign(std::istreambuf_iterator<char>(template_file), std::istreambuf_iterator<char>());
    if (script.empty()) {
        if (error) *error = "The selected VapourSynth interpolation template is missing or empty.";
        return false;
    }
    script = ReplaceAll(std::move(script), "\r\n", "\n");
    script = ReplaceAll(std::move(script), "__H_PRE__", std::to_string(pre_height));
    script = ReplaceAll(std::move(script), "__FPS_FACTOR__", Number(factor.value, 3));
    script = ReplaceAll(std::move(script), "__FPS_NUM__", std::to_string(factor.num));
    script = ReplaceAll(std::move(script), "__FPS_DEN__", std::to_string(factor.den));
    // k7sfunc types gpu as Literal[0, 1, 2]; TensorRT takes the CUDA ordinal, DirectML the DXGI index. MVTools (CPU) does
    // not use it. Never clamped or remapped (audit 7.6d): ApplyEnhancement refuses an AI stage with an index above 2.
    const std::uint32_t device = trt ? cuda_device_id_ : gpu_device_id_;
    if (mode != "mvtools" && device > kK7sfuncMaxGpuIndex) {
        if (error) *error = std::string("GPU index ") + std::to_string(device) + " is outside k7sfunc's gpu range 0-2.";
        return false;
    }
    script = ReplaceAll(std::move(script), "__GPU__", std::to_string(device));
    script = ReplaceAll(std::move(script), "__THREADS__", std::to_string(threads));
    script = ReplaceAll(std::move(script), "__SC_MODE__", std::to_string(mode == "rife" ? rife_scene_mode : scene_mode));
    script = ReplaceAll(std::move(script), "__RECAL__", recal ? "True" : "False");
    script = ReplaceAll(std::move(script), "__BLOCK__", block ? "True" : "False");
    if (trt) script = ReplaceAll(std::move(script), "__TRT_LOG__", TrtLogLiteral(data_dir_));
    script = ReplaceAll(std::move(script), "__MODEL__", std::to_string(mode == "rife" ? rife_model : drba_model));
    // RIFE_DML turbo is a bool (True = rife_v2 fast path); RIFE_NV turbo is 0/1/2: quality (ensemble) -> 0, fast -> 2.
    script = ReplaceAll(std::move(script), "__TURBO__", mode == "rife"
        ? (trt ? (rife_quality ? "0" : "2") : (rife_quality ? "False" : "True")) : std::to_string(drba_turbo));
    if (script.find("__") != std::string::npos) {
        if (error) *error = "The VapourSynth interpolation template has an unresolved setting.";
        return false;
    }
    out->script = std::move(script);
    out->signature = (trt ? "trt:" : "dml:") + mode + ":" + std::to_string(pre_height) + ":" + std::to_string(factor.num) + "/" +
        std::to_string(factor.den) + ":" + std::to_string(device) + ":" + std::to_string(threads) + ":" +
        std::to_string(mode == "rife" ? rife_scene_mode : scene_mode) + ":" + rife_id + ":" + (rife_quality ? "quality" : "fast") + ":" +
        std::to_string(drba_model) + ":" + std::to_string(drba_turbo) + ":" + (recal ? "1" : "0") + ":" + (block ? "1" : "0");
    out->template_name = Utf8(template_path.filename().wstring());
    out->backend = mode == "mvtools" ? "cpu" : trt ? "trt" : "dml";
    return true;
}

void MpvBackend::RecordShaderCompileMeasurement() {
    if (pending_shader_compile_start_.time_since_epoch().count() == 0 || pending_shader_compile_ids_.empty()) return;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - pending_shader_compile_start_).count();
    AppendLog("mpv-shader-first-frame ids=" + pending_shader_compile_ids_ + " elapsed_ms=" + Number(elapsed_ms, 3));
    for (std::size_t start = 0; start < pending_shader_compile_ids_.size();) {
        const std::size_t end = pending_shader_compile_ids_.find(',', start);
        measured_shader_ids_.insert(pending_shader_compile_ids_.substr(start,
            end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    pending_shader_compile_ids_.clear();
    pending_shader_compile_start_ = {};
}

bool MpvBackend::ApplyEnhancement(const Json& video_config) {
    if (!ctx_ || !api_ || !initialized_) return false;
    if (!enhancement_configured_) {
        EmitError("ENHANCEMENT_CATALOG_MISSING", "MPV enhancement catalog is unavailable.", false);
        return false;
    }
    AppendLog("enhancement request round6 upscale=" + video_config["upscaleMode"].str("<legacy>") +
              " interpolation=" + video_config["interpolation"]["mode"].str("<legacy>") +
              " loaded=" + std::string(file_loaded_ ? "yes" : preloading_ ? "preload" : "no"));
    {
        // video.hdrOutput (configs without it: auto). Applied by ApplyHdrOptions on the next pass for a file.
        const std::string hdr_output = video_config["hdrOutput"].str("auto");
        const std::string normalized = hdr_output == "off" || hdr_output == "on" ? hdr_output : std::string("auto");
        if (normalized != hdr_output_setting_) AppendLog("hdr output setting " + hdr_output_setting_ + " -> " + normalized);
        hdr_output_setting_ = normalized;
    }
    if (!file_loaded_ && !preloading_) {
        video_config_ = video_config;
        return true;
    }
    // A VapourSynth chain is initialising: the plan needs synchronous reads (video-params, track-list, container-fps)
    // and the vf/hwdec sets their results. Keep the newest request and apply it when the chain is up (EndVsBusy).
    if (!SyncAllowed()) {
        vs_deferred_apply_ = true;
        vs_deferred_config_ = video_config;
        AppendLog("enhancement deferred until the VapourSynth init finishes (trigger=" + vs_busy_trigger_ + ")");
        return true;
    }

    std::string upscale_mode = video_config["upscaleMode"].str();
    std::string interpolation_mode = video_config["interpolation"]["mode"].str();
    std::vector<std::string> migration_notes;
    const std::string old_preset = video_config["preset"].str("original");
    const std::string old_upscaler = video_config["upscaler"].str("off");
    const std::string old_model = video_config["ai"]["modelFile"].str();
    const std::string old_vs_up = video_config["vsUpscaleScript"].str();
    const std::string old_vs_interp = video_config["vsInterpolationScript"].str(video_config["vsScript"].str());
    const bool has_legacy_upscale = video_config["upscaleMode"].is_null() ||
        (upscale_mode == "off" && (!old_vs_up.empty() || old_upscaler != "off" ||
         (old_preset != "custom" && old_preset != "original")));
    if (has_legacy_upscale) {
        if (old_vs_up == "UAI_AJN31_HD_Balanced.vpy") upscale_mode = "dml:2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx";
        else if (old_vs_up == "UAI_AJN31_HD_Performance.vpy") upscale_mode = "dml:2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx";
        else if (old_vs_up == "UAI_AJN31_HD_Sharp_Balanced.vpy") upscale_mode = "dml:2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx";
        else if (old_vs_up == "UAI_AJN31_HD_Sharp_Performance.vpy") upscale_mode = "dml:2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx";
        else if (old_vs_up == "UAI_AJN_SD_V1beta34.vpy") upscale_mode = "dml:2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx";
        else if (old_vs_up == "MIX_UAI_DML.vpy" && !old_model.empty()) upscale_mode = "dml:" + old_model;
        else if (old_upscaler.rfind("uai_", 0) == 0 && !old_model.empty()) upscale_mode = "dml:" + old_model;
        else if (old_upscaler != "off") upscale_mode = "glsl:" + old_upscaler;
        else if (old_preset == "anime_light") upscale_mode = "glsl:cunny_fast";
        else if (old_preset == "anime_balanced") upscale_mode = "glsl:k7_anime";
        else if (old_preset == "anime_high") upscale_mode = "glsl:ani4kv2";
        else if (old_preset == "anime_low_quality") upscale_mode = "glsl:anisd";
        else if (old_preset == "live_balanced") upscale_mode = "glsl:fsrcnnx16";
        else if (old_preset == "amd_light") upscale_mode = "glsl:amd_fsr1";
        else if (old_preset == "anime_ai_dml") upscale_mode = "dml:" + (old_model.empty() ? "the_database_AnimeJaNaiV2L1_x2_fp16_op17.onnx" : old_model);
        else upscale_mode = "off";
        migration_notes.push_back("mapped legacy preset/upscaler to upscale selector");
    }
    if (interpolation_mode.empty() || interpolation_mode == "off") {
        if (old_vs_interp == "MEMC_MVT_LQ.vpy") interpolation_mode = "mvtools";
        else if (old_vs_interp == "MEMC_RIFE_DML.vpy") interpolation_mode = "rife";
        else if (old_vs_interp == "MEMC_DRBA_DML.vpy") interpolation_mode = "drba";
        else if (video_config["smooth"].str() == "on" || video_config["smooth"].str() == "auto") interpolation_mode = "mpv_smooth";
        if (interpolation_mode != "off") migration_notes.push_back("mapped legacy interpolation to interpolation selector");
    }
    if (upscale_mode.empty()) upscale_mode = "off";
    if (interpolation_mode.empty()) interpolation_mode = "off";

    const std::string legacy_preset = video_config["preset"].str("custom");
    if (legacy_preset != "custom" && legacy_preset != "original") migration_notes.push_back("preset=" + legacy_preset);
    if (old_upscaler != "off") migration_notes.push_back("legacy-upscaler=" + old_upscaler);
    if (!old_vs_up.empty()) migration_notes.push_back("legacy-vs-upscale=" + old_vs_up);
    if (!old_vs_interp.empty()) migration_notes.push_back("legacy-vs-interpolation=" + old_vs_interp);
    if (video_config["gpuBudgetGuard"].boolean(false)) migration_notes.push_back("gpuBudgetGuard=true reset to false");
    for (const auto& field : {"restore", "sharpen", "deband"}) {
        const std::string value = video_config[field].str("off");
        if (!value.empty() && value != "off") migration_notes.push_back(std::string(field) + "=" + value);
    }
    if (!round6_migration_logged_ && !migration_notes.empty()) {
        std::string dropped;
        for (const auto& item : migration_notes) dropped += (dropped.empty() ? "" : ",") + item;
        AppendLog("r6-config-migration upscale=" + upscale_mode + " interpolation=" + interpolation_mode +
                  " legacy=" + dropped + " customShaders-preserved=" +
                  std::to_string(video_config["customShaders"].items().size()));
        round6_migration_logged_ = true;
    }

    std::string model_file;
    if (upscale_mode.rfind("dml:", 0) == 0) model_file = upscale_mode.substr(4);
    std::string glsl_id;
    if (upscale_mode.rfind("glsl:", 0) == 0) glsl_id = upscale_mode.substr(5);
    if (!model_file.empty() && (model_file.find_first_of("/\\:") != std::string::npos || model_file.size() < 6 ||
        Lower(model_file.substr(model_file.size() - 5)) != ".onnx")) {
        AppendLog("r6-upscale rejected unsafe model name=" + model_file);
        EmitError("UAI_MODEL_MISSING", "無法使用這個 DirectML 模型。", false);
        model_file.clear();
    }
    if (upscale_mode != "off" && model_file.empty() && glsl_id.empty()) {
        AppendLog("r6-upscale ignored unknown selection=" + upscale_mode);
        upscale_mode = "off";
    }

    int model_scale = 2;
    std::string model_label = model_file;
    if (!model_file.empty()) {
        bool model_metadata_found = false;
        std::ifstream model_catalog_file(enhancement_root_ / L"models.json", std::ios::binary);
        const std::string model_catalog_text((std::istreambuf_iterator<char>(model_catalog_file)), std::istreambuf_iterator<char>());
        const Json model_catalog = Json::Parse(model_catalog_text);
        for (const Json& item : model_catalog["models"].items()) {
            if (item["file"].str() != model_file) continue;
            model_metadata_found = true;
            model_scale = std::clamp(static_cast<int>(item["scale"].i64(2)), 1, 4);
            model_label = item["label"].str(model_file);
            break;
        }
        if (!model_metadata_found) {
            const std::string lower_name = Lower(model_file);
            for (int candidate : {4, 3, 2, 1}) {
                if (lower_name.find("x" + std::to_string(candidate)) != std::string::npos) {
                    model_scale = candidate;
                    break;
                }
            }
        }
    }

    const Json upscale_settings = video_config["upscaleParams"];
    // upscaleParams.preHeight: "auto" (default) or an explicit 0/540/720/1080 (FMT_CTRL h_max before the model).
    // auto is resolved below, once the source height is known (AutoUpscalePreHeight). A numeric 1080 without
    // upscaleParams.preHeightVersion >= 2 is migrated to auto: it was the implicit default (models.json defaultHPre)
    // before auto existed, never a user choice; the settings UI writes preHeightVersion 2, so a 1080 chosen there stays.
    bool ai_pre_auto = true;
    int ai_pre_height = 0;
    if (upscale_settings["preHeight"].type() == Json::Type::Number) {
        const int value = static_cast<int>(upscale_settings["preHeight"].i64(0));
        const bool legacy_default = value == 1080 && upscale_settings["preHeightVersion"].i64(0) < 2;
        if (!legacy_default && (value == 0 || value == 540 || value == 720 || value == 1080)) {
            ai_pre_auto = false;
            ai_pre_height = value;
        }
    }
    const int ai_max_height = std::clamp(static_cast<int>(upscale_settings["maxHeight"].i64(0)), 0, 4320);
    const int ai_gpu_threads = std::clamp(static_cast<int>(upscale_settings["gpuThreads"].i64(2)), 1, 3);
    const bool ai_fp16 = upscale_settings["fp16"].boolean(true);

    const Json interpolation_settings = video_config["interpolation"];
    double chosen_factor = 0.0;
    int factor_num = 1;
    int factor_den = 1;
    if (!model_file.empty() || interpolation_mode == "rife" || interpolation_mode == "drba") {
        ResolveAiBackend(video_config);
    } else {
        ai_backend_.clear();
        ai_backend_reason_.clear();
    }
    const std::string ai_backend_label = ai_backend_ == "trt" ? "TensorRT" : "DirectML";
    std::string interpolation_label;
    if (interpolation_mode == "mvtools") interpolation_label = "MVTools（快速）";
    else if (interpolation_mode == "rife") {
        const std::string rife_model = interpolation_settings["rifeModel"].str("46");
        const std::string model_name = rife_model == "4251" ? "v4.25 lite" : rife_model == "426" ? "v4.26" : rife_model == "4262" ? "v4.26 heavy" : "v4.6";
        interpolation_label = "RIFE " + model_name + (interpolation_settings["rifeMode"].str("fast") == "quality" ? " 品質" : " 快速") + "（" + ai_backend_label + "）";
    } else if (interpolation_mode == "drba") interpolation_label = "DRBA（" + ai_backend_label + "）";
    else if (interpolation_mode == "mpv_smooth") interpolation_label = "mpv 平滑（非 AI）";
    else if (interpolation_mode == "amf_frc") interpolation_label = "AMD FRC（amf_frc）";

    const bool is_glsl = !glsl_id.empty();
    const bool is_dml = !model_file.empty();
    const bool wants_vs_interp = interpolation_mode == "mvtools" || interpolation_mode == "rife" || interpolation_mode == "drba";
    // mpv 平滑 needs display-resample, which resamples the audio: unavailable while bit-perfect is on (treated as off,
    // reported as interpolation_reason=bitperfect_no_smooth). The selection itself is kept for when it is turned off.
    const bool smooth_blocked = interpolation_mode == "mpv_smooth" && audio_exclusive_ == "yes";
    const bool smooth = interpolation_mode == "mpv_smooth" && !smooth_blocked;
    // AMD FRC: mpv's built-in amf_frc filter (fixed 2x, D3D11 frames on an AMD adapter, no CPU copy).
    const bool wants_amf = interpolation_mode == "amf_frc";
    std::string custom_shaders = "[";
    for (const Json& item : video_config["customShaders"].items())
        custom_shaders += (custom_shaders.size() == 1 ? "" : ",") + Json::Quote(item.str());
    custom_shaders += "]";
    const std::string normalized_config_text = "{\"preset\":\"custom\",\"restore\":\"off\",\"upscaler\":" +
        Json::Quote(is_glsl ? glsl_id : "off") + ",\"sharpen\":\"off\",\"deband\":\"off\",\"smooth\":" +
        Json::Quote(smooth ? "on" : "off") + ",\"gpuBudgetGuard\":false,\"tscale\":" +
        Json::Quote(video_config["tscale"].str("oversample")) + ",\"customShaders\":" + custom_shaders + "}";
    const Json normalized_config = Json::Parse(normalized_config_text);

    // HDR source (HdrSourceClass): unknown in on_preloaded, where mpv has no colour data yet.
    std::string hdr_source = "unknown";
    std::string hdr_primaries;
    // source_fps is the rate mpv hands a vapoursynth script as `container_fps` (vf_vapoursynth takes the frame's
    // nominal_fps, which the decoder wrapper sets from the codec's container fps): demux-fps of the default video track in
    // on_preloaded, the container-fps property afterwards (the same value once the decoder exists). Not
    // estimated-vf-fps (the filter chain's output rate: x2 with interpolation, 0 before frames flow). 0 = unknown.
    double source_fps = 0.0;
    int source_height = 0; // decoded height, else the demuxer's; 0 = unknown
    int source_width = 0;  // same source as source_height; 0 = unknown
    std::string hwdec;
    if (preloading_) {
        // on_preloaded: no decoder yet, so read the default video track's demuxer data. Colour metadata is unknown here.
        mpv_node tracks{};
        if (GetPropertySync("track-list", MPV_FORMAT_NODE, &tracks) >= 0) {
            const mpv_node* chosen = nullptr;
            if (tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list) {
                for (int i = 0; i < tracks.u.list->num; ++i) {
                    const mpv_node* track = &tracks.u.list->values[i];
                    if (NodeString(MapValue(track, "type")) != "video" || NodeBool(MapValue(track, "albumart"))) continue;
                    if (!chosen || (NodeBool(MapValue(track, "default")) && !NodeBool(MapValue(chosen, "default")))) chosen = track;
                }
            }
            if (chosen) {
                source_fps = NodeNumber(MapValue(chosen, "demux-fps"));
                source_height = static_cast<int>(NodeNumber(MapValue(chosen, "demux-h")));
                source_width = static_cast<int>(NodeNumber(MapValue(chosen, "demux-w")));
            }
            api_->free_node_contents(&tracks);
        }
        hwdec = ResolveHwdec(gpu_api_, video_config["hwdec"].str("d3d11va"));
    } else {
        mpv_node video_params{};
        if (GetPropertySync("video-params", MPV_FORMAT_NODE, &video_params) >= 0) {
            hdr_source = HdrSourceClass(NodeString(MapValue(&video_params, "gamma")), NodeNumber(MapValue(&video_params, "sig-peak")));
            hdr_primaries = NodeString(MapValue(&video_params, "primaries"));
            source_height = static_cast<int>(NodeNumber(MapValue(&video_params, "h")));
            source_width = static_cast<int>(NodeNumber(MapValue(&video_params, "w")));
            api_->free_node_contents(&video_params);
        }
        // video-params exists only once the decoder produced a frame; until then the demuxer's size.
        if (source_height <= 0 || source_width <= 0) {
            source_height = static_cast<int>(PropertyDouble("current-tracks/video/demux-h", 0.0));
            source_width = static_cast<int>(PropertyDouble("current-tracks/video/demux-w", 0.0));
        }
        source_fps = PropertyDouble("container-fps", 0.0);
        hwdec = PropertyString("hwdec-current");
    }
    // Every pass, before the plan is compiled (a rejected plan must not leave an earlier file's HDR values set).
    ApplyHdrOptions(preloading_ ? "preload" : "loaded", hdr_source, hdr_primaries);
    double average_kbps = PropertyDouble("video-bitrate", 0.0) / 1000.0;
    if (average_kbps <= 0.0) {
        const double duration = PropertyDouble("duration", 0.0);
        const double bytes = PropertyDouble("file-size", 0.0);
        if (duration > 0.0 && bytes > 0.0) average_kbps = bytes * 8.0 / duration / 1000.0;
    }
    // Upscale H_Pre auto: min(source height, ceil(H_Max / model scale)), H_Max = upscaleParams.maxHeight or the monitor
    // height. 1080p source, x2 model: 720 on a 1440p display (PlayKit's default), 1080 on 4K, 540 on 1080p.
    // OnProperty("video-params") re-applies if the decoded height changes the result.
    const int ai_pre_hmax = ai_max_height > 0 ? ai_max_height : MonitorHeight(host_);
    if (ai_pre_auto) ai_pre_height = AutoUpscalePreHeight(source_height, ai_pre_hmax, model_scale);
    const std::string ai_pre_note = std::to_string(ai_pre_height) + (ai_pre_auto
        ? " (auto: hmax=" + std::to_string(ai_pre_hmax) + ", scale=" + std::to_string(model_scale) + ", src=" +
          (source_height > 0 ? std::to_string(source_height) : std::string("unknown")) + ")"
        : std::string(" (explicit)"));
    const std::string gpu_api = video_config["gpuApi"].str("d3d11");
    const double display_fps = EffectiveDisplayHz();
    const double speed = PropertyDouble("speed", 1.0);
    EnhancementPlan plan;
    std::string error;
    if (!enhancement_compiler_.Compile(normalized_config, gpu_api, gpu_vendor_, enhancement_root_ / L"shaders",
                                       exe_dir_ / L"vs-runtime", source_fps, display_fps, speed, hwdec,
                                       average_kbps, IsHdrSourceClass(hdr_source),
                                       &plan, &error)) {
        AppendLog("enhancement rejected: " + error);
        EmitError(error.find("budget") != std::string::npos ? "GPU_BUDGET" : "ENHANCEMENT_REJECTED", error, false);
        return false;
    }
    for (const Json& item : video_config["customShaders"].items()) {
        std::string relative = item.str();
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const bool safe = !relative.empty() && relative.find("..") == std::string::npos && relative.find(':') == std::string::npos && relative.front() != '/';
        const std::filesystem::path full = enhancement_root_ / L"shaders" /
            std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(relative.data()), relative.size()));
        std::error_code ec;
        if (!safe || !std::filesystem::is_regular_file(full, ec)) {
            AppendLog("custom shader rejected or missing: " + relative);
            continue;
        }
        const auto utf8 = full.u8string();
        plan.shader_paths.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        plan.shader_ids.push_back("custom:" + relative);
    }
    std::string shader_ids;
    for (const auto& id : plan.shader_ids) {
        if (!shader_ids.empty()) shader_ids += ',';
        shader_ids += id;
    }
    AppendLog("enhancement plan preset=custom upscale=" + (is_dml ? model_file : is_glsl ? glsl_id : std::string("off")) + " shaders=" +
              (shader_ids.empty() ? std::string("none") : shader_ids) + " cost_ms=" + Number(plan.estimated_cost_ms) +
              " reason=" + plan.reason);

    plan.filter.clear();
    plan.ai_dml = false;
    round6_upscale_mode_ = upscale_mode;
    round6_upscale_model_ = model_file;
    round6_upscale_label_ = is_dml ? model_label : glsl_id;
    round6_upscale_backend_ = is_dml ? ai_backend_label : is_glsl ? "GLSL" : std::string();
    round6_upscale_scale_ = is_dml ? model_scale : std::max(1, plan.upscale_scale);
    round6_upscale_pre_height_ = is_dml ? ai_pre_height : 0;
    round6_upscale_pre_auto_ = is_dml && ai_pre_auto;
    round6_upscale_pre_source_height_ = source_height;
    round6_upscale_pre_hmax_ = ai_pre_hmax;
    round6_upscale_max_height_ = ai_max_height;
    round6_upscale_active_ = is_glsl && plan.upscale != "off";
    round6_interpolation_mode_ = interpolation_mode;
    round6_interpolation_label_ = interpolation_label;
    round6_interpolation_factor_ = 0.0;
    round6_interpolation_fps_num_ = 1;
    round6_interpolation_fps_den_ = 1;
    round6_interpolation_pre_height_ = 0;
    round6_interpolation_pre_auto_ = false;
    round6_interpolation_active_ = smooth;
    round6_interpolation_reason_.clear();
    if (smooth_blocked) round6_interpolation_reason_ = "bitperfect_no_smooth";
    round6_upscale_reason_.clear();
    vs_up_script_.clear();
    vs_fi_script_.clear();
    vs_chain_script_.clear();
    vs_chain_order_.clear();
    std::vector<std::pair<std::string, std::string>> generated_filters;
    vs_script_.clear();
    // File name of a generated script (up_/fi_/chain_<hash>.vpy): what a VS traceback names.
    const auto script_file_name = [](const std::string& filter) {
        const std::string path = VapourSynthGeneratedPath(filter);
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };

    // Both VapourSynth stages are prepared first (substituted templates); then either one chain script holds both
    // (plan section 5.7 rule 3, mpv_PlayKit Discussion #313: one VS core, one inference runtime, no VS -> mpv -> VS
    // round trip between the stages, one buffered-frames x concurrent-frames input queue, one reload per seek), or
    // each stage gets its own script and vapoursynth filter as before.
    VsStageScript up_stage;
    VsStageScript fi_stage;
    bool up_ready = false;
    std::string up_preflight_reason; // 7.5b / 7.6d preflight result ("" = not refused)
    // Audit 7.6d: the AI stages take one device index (TensorRT: the CUDA ordinal; DirectML: the hardware DXGI index),
    // and k7sfunc accepts only 0, 1, 2 for it (kK7sfuncMaxGpuIndex). A larger index is refused with a stated reason before
    // any script exists: never clamped, never moved to another GPU. (The DirectML index skips software adapters, as
    // onnxruntime's EnumAdapters1 order does while WARP enumerates last.) MVTools (CPU) and amf_frc are not affected.
    const bool wants_ai_interp = interpolation_mode == "rife" || interpolation_mode == "drba";
    const std::uint32_t ai_device = ai_backend_ == "trt" ? cuda_device_id_ : gpu_device_id_;
    std::string adapter_refusal;
    if ((is_dml || wants_ai_interp) && ai_device > kK7sfuncMaxGpuIndex) {
        adapter_refusal = "adapter_index_unsupported";
        const std::string which = ai_backend_ == "trt" ? "CUDA 裝置序號 " + std::to_string(cuda_device_id_)
                                                       : "DXGI 硬體序號 " + std::to_string(gpu_device_id_);
        AppendLog("r6-ai preflight refused reason=adapter_index_unsupported backend=" + ai_backend_ + " device=" + std::to_string(ai_device) +
                  " (dxgi_hw_index=" + std::to_string(gpu_device_id_) + " cuda_index=" + std::to_string(cuda_device_id_) +
                  ") adapter=" + active_adapter_ + "; k7sfunc gpu accepts 0-2; no clamp, no other adapter" +
                  (is_dml ? std::string(" upscale=refused") : std::string()) + (wants_ai_interp ? std::string(" interpolation=refused") : std::string()));
        if (vs_failure_notified_.insert("preflight|adapter_index_unsupported").second)
            EmitError("AI_ENHANCEMENT_UNSUPPORTED", "所選 GPU（" + which + "）超出 k7sfunc 支援的 0–2，" +
                      std::string(is_dml && wants_ai_interp ? "AI 放大與 AI 補幀" : is_dml ? "AI 放大" : "AI 補幀") +
                      "在此 GPU 上無法使用；不會改用其他 GPU。", false);
    }
    if (is_dml) {
        plan.ai_dml = true;
        plan.ai_model_file = model_file;
        plan.ai_pre_height = ai_pre_height;
        plan.ai_max_height = ai_max_height;
        plan.ai_gpu_threads = ai_gpu_threads;
        plan.ai_fp16 = ai_fp16;
        plan.upscale = "dml:" + model_file;
        plan.upscale_scale = model_scale;
        plan.active = true;
        // Audit 7.5b: TensorRT supported-shape preflight. k7sfunc UAI_NV_TRT builds its dynamic engine with
        // min_shapes=[384, 384] (width, height; K7/mod_mix.py UAI_NV_TRT), so a frame below 384 in either dimension
        // fails the engine build or the inference. Checked on the frame UAI actually receives: the source after
        // FMT_CTRL(h_max=H_Pre), which only shrinks (aspect kept, even sizes for subsampled formats). Not pre-upscaled
        // to 384 (an extra resample) and not silently moved to DirectML (whose ONNX Runtime path has no such minimum):
        // the upscale stage is left out with a stated reason; the rest of the chain runs. Unknown size: no gate.
        std::string trt_shape_refusal;
        if (adapter_refusal.empty() && ai_backend_ == "trt" && source_width > 0 && source_height > 0) {
            int in_w = source_width;
            int in_h = source_height;
            if (ai_pre_height > 0 && source_height > ai_pre_height) {
                in_h = ai_pre_height / 2 * 2;
                in_w = static_cast<int>(std::floor(static_cast<double>(source_width) * ai_pre_height / source_height / 2.0)) * 2;
            }
            constexpr int kTrtMinShape = 384;
            if (in_w < kTrtMinShape || in_h < kTrtMinShape) {
                trt_shape_refusal = std::to_string(in_w) + "x" + std::to_string(in_h);
                AppendLog("r6-upscale preflight refused backend=trt reason=trt_shape_lt_384 input=" + trt_shape_refusal +
                          " source=" + std::to_string(source_width) + "x" + std::to_string(source_height) + " h_pre=" + ai_pre_note +
                          " (k7sfunc UAI_NV_TRT min_shapes=[384,384]); no pre-upscale, no DirectML substitution");
                if (vs_failure_notified_.insert("preflight|trt_shape_lt_384").second)
                    EmitError("AI_ENHANCEMENT_UNSUPPORTED", "TensorRT 放大需要輸入寬高皆 ≥ 384（此檔送入模型為 " + trt_shape_refusal +
                              "），此檔不使用 AI 放大" + std::string(wants_vs_interp || wants_amf ? "；補幀照常。" : "。"), false);
            } else {
                AppendLog("r6-upscale preflight trt input=" + std::to_string(in_w) + "x" + std::to_string(in_h) + " ok (>= 384)");
            }
        }
        if (!adapter_refusal.empty()) {
            plan.ai_dml = false;
            plan.upscale = "off";
            plan.upscale_scale = 1;
            round6_upscale_reason_ = adapter_refusal;
            up_preflight_reason = adapter_refusal;
        } else if (!trt_shape_refusal.empty()) {
            plan.ai_dml = false;
            plan.upscale = "off";
            plan.upscale_scale = 1;
            round6_upscale_reason_ = "trt_shape_lt_384";
            up_preflight_reason = "trt_shape_lt_384";
        } else if (PrepareUpscaleScript(plan, &up_stage, &error)) {
            up_ready = true;
        } else {
            AppendLog("r6-upscale unavailable model=" + model_file + " error=" + error);
            EmitError("AI_ENHANCEMENT_UNAVAILABLE", ai_backend_label + " 放大目前無法使用：" + error, false);
            plan.ai_dml = false;
            plan.upscale = "off";
            plan.upscale_scale = 1;
            error.clear();
        }
    } else if (is_glsl) {
        round6_upscale_label_ = glsl_id;
    }
    const auto upscale_unavailable = [&](const std::string& why) {
        AppendLog("r6-upscale unavailable model=" + model_file + " error=" + why);
        EmitError("AI_ENHANCEMENT_UNAVAILABLE", ai_backend_label + " 放大目前無法使用：" + why, false);
        round6_upscale_active_ = false;
        plan.ai_dml = false;
        plan.upscale = "off";
        plan.upscale_scale = 1;
    };

    bool fi_ready = false;
    // H_PRE auto needs to know whether a VapourSynth upscaler precedes this stage and the display cap it uses.
    const bool after_vs_upscale = is_dml && up_ready;
    // Audit 7.2a: unsupported-interpolation preflight. Every VapourSynth interpolation body (RIFE, DRBA, MVTools; DML
    // and TRT) starts with k7sfunc FPS_CTRL(fps_in=container_fps, fps_ret=True), which raises above fps_max=32
    // (K7/mod_fmt.py FPS_CTRL). So a source whose fps is KNOWN to be above 32 never gets the stage: no script, no
    // failed-script toast, no CPU-frame readback for it; the upscale (if any) runs alone. Unknown fps (0) is not refused
    // here: it is decided by the next pass that knows the fps (FILE_LOADED re-reads container-fps), and the chain keeps
    // the two-script split below for it, so a failure there cannot take the upscale down.
    // Core-reliability 3A: the rate the script actually receives. With mpv's auto deinterlacer active (deint_seen_) the
    // VapourSynth input is the field rate, 2x container-fps (bwdif send_field), while the container-fps property is
    // unchanged; vs_fps_limit_seen_: FPS_CTRL already refused this file's input rate at the first frame. That refusal with a
    // container rate <= 32 also means the field rate: mpv's deinterlacer is the only stage before vf that changes the
    // rate, and deinterlace-active can be reported after the busy period's deferred pass (measured: MPEG-2, RIFE only).
    const bool deint_rate = (deint_seen_ || vs_fps_limit_seen_) && source_fps > 0.0;
    const double vs_input_fps = deint_rate ? source_fps * 2.0 : source_fps;
    std::string fi_preflight_reason;
    if (wants_ai_interp && !adapter_refusal.empty()) {
        fi_preflight_reason = adapter_refusal;
        round6_interpolation_active_ = false;
        round6_interpolation_reason_ = adapter_refusal;
    } else if (wants_vs_interp && source_fps > kVsInterpolationMaxFps) {
        fi_preflight_reason = "fps_gt_32";
        AppendLog("r6-interpolation preflight refused mode=" + interpolation_mode + " reason=fps_gt_32 source_fps=" + Number(source_fps) +
                  " signal=" + (preloading_ ? "demux-fps" : "container-fps") + " (k7sfunc FPS_CTRL fps_max=32)");
        if (vs_failure_notified_.insert("preflight|fps_gt_32").second)
            EmitError("INTERPOLATION_UNSUPPORTED", "來源 " + Number(source_fps, 2) + " fps 高於 32 fps，" +
                      (interpolation_label.empty() ? std::string("補幀") : interpolation_label) + " 不支援（上限 32 fps），以原始幀率播放" +
                      std::string(is_dml && up_ready ? "；AI 放大照常。" : "。"), false);
        round6_interpolation_active_ = false;
        round6_interpolation_reason_ = "fps_gt_32";
    } else if (wants_vs_interp && (vs_input_fps > kVsInterpolationMaxFps || vs_fps_limit_seen_)) {
        // 3A: same refusal as above, for the deinterlaced rate (upscale-only continuation, no failed-script toast).
        const std::string reason = deint_rate ? "deint_fps_gt_32" : "fps_gt_32";
        fi_preflight_reason = reason;
        AppendLog("r6-interpolation preflight refused mode=" + interpolation_mode + " reason=" + reason + " source_fps=" + Number(source_fps) +
                  " vs_input_fps=" + Number(vs_input_fps) + " signal=" +
                  (deint_rate ? std::string("container-fps x2 (field rate; ") + (deint_seen_ ? "deinterlace-active)" : "deinterlace-active not reported yet)")
                              : std::string("container-fps")) +
                  (vs_fps_limit_seen_ ? " fps-limit-seen (FPS_CTRL refused the script input)" : "") + " (k7sfunc FPS_CTRL fps_max=32)");
        if (vs_failure_notified_.insert("preflight|" + reason).second) {
            const std::string tail = is_dml && up_ready ? std::string("；AI 放大照常。")
                : deint_rate ? "，以 " + Number(vs_input_fps, 2) + " fps 播放。" : std::string("。");
            EmitError("INTERPOLATION_UNSUPPORTED", deint_rate
                ? "反交錯後 " + Number(vs_input_fps, 2) + " fps 高於 32 fps，補幀已停用" + tail
                : "補幀腳本收到的影格率高於 32 fps（k7sfunc 上限），補幀已停用" + tail, false);
        }
        round6_interpolation_active_ = false;
        round6_interpolation_reason_ = reason;
    }
    if (wants_vs_interp && fi_preflight_reason.empty()) {
        const int display_cap_height = ai_max_height > 0 ? ai_max_height : MonitorHeight(host_);
        if (PrepareInterpolationScript(interpolation_settings, interpolation_mode, vs_input_fps, display_fps,
                                       &fi_stage, &error, &chosen_factor, &factor_num, &factor_den,
                                       after_vs_upscale, display_cap_height,
                                       &round6_interpolation_pre_height_, &round6_interpolation_pre_auto_)) {
            fi_ready = true;
        } else {
            AppendLog("r6-interpolation unavailable mode=" + interpolation_mode + " error=" + error);
            EmitError("INTERPOLATION_UNAVAILABLE", "補幀目前無法使用：" + error, false);
            round6_interpolation_active_ = false;
            error.clear();
        }
    }

    // One chain script when both stages are VapourSynth on the same inference backend (MVTools is CPU-only and fits
    // either). Two scripts otherwise, with the reason logged:
    //  - different backends (cannot happen with one aiBackend today; guarded so a future per-stage backend still works);
    //  - source fps unknown or above 32: the interpolation body starts with FPS_CTRL(fps_ret=True), which raises above
    //    32 fps; in one script that would also take the upscale down, while two scripts keep the upscale running.
    std::string chain_filter;
    if (up_ready && fi_ready) {
        std::string why_not;
        VsSections up_sections;
        VsSections fi_sections;
        if (fi_stage.backend != "cpu" && fi_stage.backend != up_stage.backend)
            why_not = "backends differ (upscale=" + up_stage.backend + " interpolation=" + fi_stage.backend + ")";
        else if (!(vs_input_fps > 0.0 && vs_input_fps <= 32.0))
            why_not = "source fps " + (vs_input_fps > 0.0 ? Number(vs_input_fps) : std::string("unknown")) +
                      " not in (0, 32] (FPS_CTRL in the interpolation body would abort the upscale too)";
        else if (!SplitVsSections(up_stage.script, &up_sections) || !SplitVsSections(fi_stage.script, &fi_sections))
            why_not = "template sections missing (" + up_stage.template_name + " / " + fi_stage.template_name + ")";
        if (why_not.empty()) {
            // The TensorRT prologue in the upscale header tags vs-trt.log lines with its template name: name both.
            const std::string header = ReplaceAll(up_sections.header, "\"" + up_stage.template_name + "\"",
                                                  "\"" + up_stage.template_name + " + " + fi_stage.template_name + "\"");
            const std::string script = header +
                "# --- upscale: " + up_stage.template_name + " ---\n" + up_sections.body +
                "# --- interpolation: " + fi_stage.template_name + " ---\n" + fi_sections.body +
                "# --- footer ---\n" + up_sections.footer;
            if (!WriteVsScript("chain", "chain|" + up_stage.signature + "|" + fi_stage.signature, script, &chain_filter, &error)) {
                why_not = "write failed: " + error;
                error.clear();
                chain_filter.clear();
            }
        }
        if (!why_not.empty()) AppendLog("r6-vf chain not used, one script per stage: " + why_not);
    }

    const auto interpolation_succeeded = [&] {
        if (interpolation_mode == "rife") {
            const std::string rife_id = interpolation_settings["rifeModel"].str("46");
            const std::string model_label = rife_id == "4251" ? "v4.25 lite" : rife_id == "426" ? "v4.26" : rife_id == "4262" ? "v4.26 heavy" : "v4.6";
            interpolation_label = "RIFE " + model_label + (interpolation_settings["rifeMode"].str("fast") == "quality" ? " 品質" : " 快速") + "（" + ai_backend_label + "）";
        }
        round6_interpolation_label_ = interpolation_label;
        round6_interpolation_factor_ = chosen_factor;
        round6_interpolation_fps_num_ = factor_num;
        round6_interpolation_fps_den_ = factor_den;
        round6_interpolation_active_ = true;
    };
    const std::string fi_h_pre_note = std::to_string(round6_interpolation_pre_height_) + (round6_interpolation_pre_auto_ ? " (auto)" : "");
    if (!chain_filter.empty()) {
        generated_filters.emplace_back("chain", chain_filter);
        round6_upscale_active_ = true;
        interpolation_succeeded();
        vs_chain_script_ = script_file_name(chain_filter);
        vs_up_script_ = vs_chain_script_;
        vs_fi_script_ = vs_chain_script_;
        AppendLog("r6-vf slot=chain backend=" + up_stage.backend + " stages=" + up_stage.template_name + "+" + fi_stage.template_name +
                  " model=" + model_file + " h_pre=" + ai_pre_note + " interpolation=" + interpolation_mode +
                  " label=" + round6_interpolation_label_ + " factor=" + std::to_string(factor_num) + "/" + std::to_string(factor_den) +
                  " fi_h_pre=" + fi_h_pre_note + " generated=" + VapourSynthGeneratedPath(chain_filter) + " vf=" + chain_filter);
    } else {
        if (up_ready) {
            std::string upscale_filter;
            if (WriteVsScript("up", up_stage.signature, up_stage.script, &upscale_filter, &error)) {
                generated_filters.emplace_back("upscale", upscale_filter);
                round6_upscale_active_ = true;
                vs_up_script_ = script_file_name(upscale_filter);
                AppendLog("r6-vf slot=upscale backend=" + ai_backend_ + " model=" + model_file + " h_pre=" + ai_pre_note +
                          " generated=" + VapourSynthGeneratedPath(upscale_filter) + " vf=" + upscale_filter);
            } else {
                upscale_unavailable(error);
                error.clear();
            }
        }
        if (fi_ready) {
            std::string interpolation_filter;
            if (WriteVsScript("fi", fi_stage.signature, fi_stage.script, &interpolation_filter, &error)) {
                generated_filters.emplace_back("interpolation", interpolation_filter);
                vs_fi_script_ = script_file_name(interpolation_filter);
                interpolation_succeeded();
                AppendLog("r6-vf slot=interpolation mode=" + interpolation_mode + " backend=" + fi_stage.backend + " label=" + round6_interpolation_label_ +
                          " generated=" + VapourSynthGeneratedPath(interpolation_filter) + " vf=" + interpolation_filter + " factor=" +
                          std::to_string(factor_num) + "/" + std::to_string(factor_den) + " h_pre=" + fi_h_pre_note +
                          " after=" + (after_vs_upscale ? std::string("upscale") : std::string("decoder")));
            } else {
                AppendLog("r6-interpolation unavailable mode=" + interpolation_mode + " error=" + error);
                EmitError("INTERPOLATION_UNAVAILABLE", "補幀目前無法使用：" + error, false);
                round6_interpolation_active_ = false;
                error.clear();
            }
        }
    }
    if (smooth) round6_interpolation_factor_ = 1.0;
    // Audit 7.2c: when EVERY generated VapourSynth script already failed in this file (vs_failed_scripts_), none is put
    // back into vf: mpv would only keep a bypassed dead filter, and re-adding it after a vf clear would re-run the failing
    // script (under TensorRT another engine-build attempt). The script names stay recorded, so ApplyVsFailureOverrides
    // keeps the panel reasons; with no VS stage left, hwdec returns to the configured decoder below (hwdec first, then
    // vf), and an amf_frc tail becomes zero-copy amf_frc alone. With any stage still working nothing changes: mpv
    // bypasses the dead one for free, the survivor needs CPU frames anyway, and an identical vf keeps the survivor loaded.
    // A different script (new settings, new signature) is not suppressed: it is tried once, then it is in the set too.
    bool vs_dead_suppressed = false;
    if (!generated_filters.empty() &&
        std::all_of(generated_filters.begin(), generated_filters.end(), [&](const auto& entry) {
            return vs_failed_scripts_.count(script_file_name(entry.second)) != 0;
        })) {
        std::string names;
        for (const auto& [slot, filter] : generated_filters) names += (names.empty() ? "" : ",") + slot + ":" + script_file_name(filter);
        generated_filters.clear();
        if (is_dml) { // as for the other unavailable-upscale paths (the scaler choice below is unchanged by this)
            plan.ai_dml = false;
            plan.upscale = "off";
            plan.upscale_scale = 1;
        }
        ApplyVsFailureOverrides(); // stage flags off, reasons vs_*_failed (the names above are vs_up/fi/chain_script_)
        vs_dead_suppressed = true;
        vs_dead_removed_ = true;
        AppendLog("vs-dead-cleanup every VapourSynth stage failed in this file (" + names + "): not re-added to vf; " +
                  "the configured decoder is restored" + (wants_amf ? std::string(", amf_frc runs alone") : std::string()));
    }
    for (const auto& [slot, filter] : generated_filters) {
        if (!plan.filter.empty()) plan.filter += ',';
        plan.filter += filter;
        vs_chain_order_.push_back(slot);
        vs_script_ += (vs_script_.empty() ? "" : " + ") + (slot == "chain" ? std::string("upscale + interpolation (one script)") : slot);
    }
    // AMD FRC runs after the optional VapourSynth upscaler (same upscale -> interpolation order as the VS modes).
    // Gated on the renderer fixed at startup: amf_frc needs D3D11 frames, which gpu-api=vulkan never provides.
    std::string amf_filter;
    std::string amf_chain;
    if (wants_amf) {
        if (gpu_vendor_ != "amd") round6_interpolation_reason_ = "amf_not_amd";
        else if (gpu_api_ != "d3d11") round6_interpolation_reason_ = "amf_needs_d3d11";
        if (!round6_interpolation_reason_.empty()) {
            AppendLog("r6-interpolation unavailable mode=amf_frc reason=" + round6_interpolation_reason_ + " vendor=" + gpu_vendor_ +
                      " gpu-api=" + gpu_api_);
        } else {
            std::string profile = interpolation_settings["amfProfile"].str("auto");
            if (profile != "auto" && profile != "low" && profile != "high" && profile != "super") profile = "auto";
            std::string mv_search = interpolation_settings["amfMvSearch"].str("native");
            if (mv_search != "native" && mv_search != "performance") mv_search = "native";
            // Explicit options for the log; future-frame stays at mpv's default (yes), indicator/fallback stay off.
            amf_filter = "amf_frc=profile=" + profile + ":mv-search=" + mv_search;
            // With a VapourSynth stage the frames are in system memory (auto-copy): upload them back to D3D11 for AMF.
            amf_chain = plan.filter.empty() ? amf_filter : "format=d3d11," + amf_filter;
            if (!plan.filter.empty()) plan.filter += ',';
            plan.filter += amf_chain;
            if (amf_chain != amf_filter) vs_chain_order_.push_back("format");
            vs_chain_order_.push_back("amf_frc");
            round6_interpolation_factor_ = 2.0;
            round6_interpolation_fps_num_ = 2;
            round6_interpolation_fps_den_ = 1;
            round6_interpolation_active_ = true; // cleared below if mpv rejects the chain
            AppendLog("r6-vf slot=interpolation mode=amf_frc backend=amf label=" + round6_interpolation_label_ + " vf=" + amf_chain +
                      " factor=2/1 source_fps=" + Number(source_fps) + " display_fps=" + Number(display_fps) +
                      " after=" + (vs_script_.empty() ? std::string("decoder") : vs_script_));
        }
    }
    if (!plan.filter.empty()) AppendLog("r6-vf reaching mpv chain=" + plan.filter);

    if (round6_upscale_active_ || round6_interpolation_active_) {
        plan.active = true;
        plan.smooth_active = false;
    }
    plan.active = !plan.shader_paths.empty() || plan.restore != "off" || plan.upscale != "off" ||
        plan.sharpen != "off" || plan.deband != "off" || plan.smooth_active || plan.deband_active || plan.hdr_active ||
        round6_upscale_active_ || round6_interpolation_active_;
    if (interpolation_settings["factor"].str("auto") == "auto") {
        round6_last_auto_source_fps_ = source_fps;
        round6_last_auto_display_hz_ = display_fps;
    }

    // VapourSynth works on system-memory frames: auto-copy (d3d11va-copy under both APIs). Otherwise the
    // configured decoder resolved for the renderer API fixed at startup (vulkan -> Vulkan Video). amf_frc alone
    // keeps that default (d3d11va), so AMD FRC consumes the decoder's D3D11 frames with no copy.
    //
    // Each structural option is written only when it differs from what was last set, so the
    // FILE_LOADED pass after on_preloaded (and display-fps updates) does not rebuild the chain.
    // amf_frc and the TensorRT stages take the same path as the DirectML ones.
    //
    // Exception, measured with DRBA on a 1080p HEVC file resumed at 60 s: when the open starts with a
    // precise seek, creating the decoder in copy-back mode with the VapourSynth chain already attached
    // makes the whole keyframe-to-start preroll go through GPU->CPU copies and the filter (first frame
    // 2.3-4.6 s, against 1.1 s when both are switched at FILE_LOADED as before). So a resume open keeps
    // zero-copy decoding and no VapourSynth chain in on_preloaded, and the FILE_LOADED pass adds them
    // (a chain with amf_frc after a VapourSynth stage is deferred as a whole; amf_frc alone is not).
    //
    // Audit 7.2b: CPU frames only for a VapourSynth stage that is actually in the chain. A stage refused by a preflight
    // (fps_gt_32, trt_shape_lt_384) or unavailable is not active, so with no other VS stage the native decoder path
    // (d3d11va) is kept or restored, hwdec first and vf second as always (an amf_frc tail then gets D3D11 frames
    // straight from the decoder). A VS stage that failed at runtime keeps auto-copy here (7.2c, not this change).
    const bool vs_filter_active = (round6_upscale_active_ && is_dml) || (round6_interpolation_active_ && wants_vs_interp);
    const bool vs_chain = plan.filter.find("vapoursynth") != std::string::npos;
    const bool defer_vs_chain = preloading_ && open_start_ms_ > 0 && vs_chain;
    const std::string wanted_hwdec = (!defer_vs_chain && vs_filter_active)
        ? std::string("auto-copy") : ResolveHwdec(gpu_api_, video_config["hwdec"].str("d3d11va"));
    std::string wanted_vf = defer_vs_chain ? std::string() : plan.filter;
    if (defer_vs_chain) AppendLog("r6-vf deferred to FILE_LOADED (resume at " + std::to_string(open_start_ms_) + " ms)");
    const std::string hwdec_why = vs_filter_active
        ? std::string(defer_vs_chain ? "vs-stage-deferred" : "vs-stage-needs-cpu-frames") + "(" +
              (round6_upscale_active_ && is_dml ? std::string("upscale") : std::string()) +
              (round6_upscale_active_ && is_dml && round6_interpolation_active_ && wants_vs_interp ? "+" : "") +
              (round6_interpolation_active_ && wants_vs_interp ? std::string("interpolation") : std::string()) + ")"
        : std::string("no-vs-stage") +
              (!fi_preflight_reason.empty() ? "(interpolation refused: " + fi_preflight_reason + ")" : std::string()) +
              (!up_preflight_reason.empty() ? "(upscale refused: " + up_preflight_reason + ")" : std::string()) +
              (vs_dead_suppressed ? std::string("(all VS stages failed: vs-dead-cleanup)") : std::string());
    const auto previous_hwdec = applied_options_.find("hwdec");
    const bool hwdec_changes = previous_hwdec == applied_options_.end() || previous_hwdec->second != wanted_hwdec;
    if (!SetOptionIfChanged("hwdec", wanted_hwdec))
        AppendLog("could not change hwdec live; current decoder will use the option on the next file open");
    else
        AppendLog("hwdec requested=" + wanted_hwdec + " gpu-api=" + gpu_api_ + " reason=" + hwdec_why +
                  (hwdec_changes ? std::string(" (changed)") : std::string(" (unchanged)")));
    // AMD FRC rejected by mpv: drop it and keep whatever else was requested (vf stays cleared if that was all).
    // The rejected chain is remembered so the FILE_LOADED pass does not try it (and rebuild the chain) again.
    const auto drop_amf = [&] {
        round6_interpolation_active_ = false;
        round6_interpolation_factor_ = 0.0;
        round6_interpolation_fps_num_ = 1;
        round6_interpolation_fps_den_ = 1;
        round6_interpolation_reason_ = "amf_failed";
        plan.filter.erase(plan.filter.size() - amf_chain.size());
        if (!plan.filter.empty() && plan.filter.back() == ',') plan.filter.pop_back();
        plan.active = !plan.shader_paths.empty() || plan.upscale != "off" || plan.deband_active || plan.hdr_active || round6_upscale_active_;
        wanted_vf = plan.filter;
    };
    const auto rejected_vf = applied_options_.find("vf:rejected");
    if (!amf_chain.empty() && !wanted_vf.empty() && rejected_vf != applied_options_.end() && rejected_vf->second == wanted_vf) drop_amf();
    const auto applied_vf = applied_options_.find("vf");
    const bool vf_changed = applied_vf == applied_options_.end() || applied_vf->second != wanted_vf;
    if (vf_changed) {
        // One synchronous `vf set` (the property) replaces the chain in a single filter reinit and
        // also works inside the on_preloaded hook; an empty value clears it.
        applied_options_.erase("vf");
        if (!SetPropertyString("vf", wanted_vf)) {
            if (wanted_vf.empty()) {
                EmitError("ENHANCEMENT_FILTER", "Could not clear the previous video filter chain.", false);
                return false;
            }
            AppendLog("r6-vf rejected by mpv chain=" + wanted_vf);
            EmitError("ENHANCEMENT_FILTER", "Could not add the requested MPV video filter.", false);
            if (amf_chain.empty()) return false;
            applied_options_["vf:rejected"] = wanted_vf;
            drop_amf();
            if (!SetPropertyString("vf", wanted_vf)) {
                SetPropertyString("vf", "");
                applied_options_["vf"] = "";
                return false;
            }
            AppendLog("r6-interpolation amf_frc dropped after rejection; remaining chain=" + (wanted_vf.empty() ? std::string("<none>") : wanted_vf));
        }
        applied_options_["vf"] = wanted_vf;
        AppendLog(std::string("r6-vf applied phase=") + (preloading_ ? "preload" : "loaded") + " chain=" + (wanted_vf.empty() ? "<empty>" : wanted_vf));
    }
    // The vf set above is synchronous (its result drives the amf_frc fallback) and returns before any script runs:
    // mpv's vapoursynth filter evaluates its script when the first frame reaches it, on the playback thread. From here
    // on nothing may wait for that thread: every new file in on_preloaded (the chain is built for it even when the vf
    // string is unchanged), and a changed chain on a loaded file.
    if (wanted_vf.find("vapoursynth") != std::string::npos && (preloading_ || (vf_changed && file_loaded_)))
        BeginVsBusy(preloading_ || first_frame_pending_ ? "load" : "apply");

    std::vector<mpv_node> shader_nodes(plan.shader_paths.size());
    for (std::size_t i = 0; i < plan.shader_paths.size(); ++i) {
        shader_nodes[i].format = MPV_FORMAT_STRING;
        shader_nodes[i].u.string = const_cast<char*>(plan.shader_paths[i].c_str());
    }
    mpv_node_list shader_list{};
    shader_list.num = static_cast<int>(shader_nodes.size());
    shader_list.values = shader_nodes.empty() ? nullptr : shader_nodes.data();
    mpv_node shader_value{};
    shader_value.format = MPV_FORMAT_NODE_ARRAY;
    shader_value.u.list = &shader_list;
    std::string shader_key;
    for (const auto& path : plan.shader_paths) shader_key += path + '\n';
    const auto applied_shaders = applied_options_.find("glsl-shaders");
    const bool shaders_changed = applied_shaders == applied_options_.end() || applied_shaders->second != shader_key;
    if (shaders_changed && !plan.shader_paths.empty()) {
        std::string pending_ids;
        for (const auto& id : plan.shader_ids) {
            if (measured_shader_ids_.find(id) != measured_shader_ids_.end()) continue;
            if (!pending_ids.empty()) pending_ids += ',';
            pending_ids += id;
        }
        if (!pending_ids.empty()) {
            pending_shader_compile_ids_ = std::move(pending_ids);
            pending_shader_compile_start_ = std::chrono::steady_clock::now();
        }
    }
    if (!shaders_changed) {
        // unchanged chain: nothing to recompile
    } else if (plan.shader_paths.empty()) {
        applied_options_.erase("glsl-shaders");
        if (!SetPropertyString("glsl-shaders", "")) {
            EmitError("ENHANCEMENT_SHADER", "Could not clear the previous MPV shader chain.", false);
            return false;
        }
    } else {
        applied_options_.erase("glsl-shaders");
        const int shader_result = SetPropertyNode("glsl-shaders", &shader_value);
        if (shader_result < 0) {
            const std::string detail = MpvErrorText(api_->error_string, shader_result);
            EmitError("ENHANCEMENT_SHADER", "Could not apply shader chain: " + detail, false);
            return false;
        }
    }
    if (shaders_changed) applied_options_["glsl-shaders"] = shader_key;

    const bool has_upscale = upscale_mode != "off";
    SetOptionIfChanged("scale", has_upscale ? "bilinear" : "ewa_lanczossharp");
    SetOptionIfChanged("cscale", has_upscale ? "bilinear" : "lanczos");
    SetOptionIfChanged("dscale", has_upscale ? "bicubic" : "mitchell");
    SetOptionIfChanged("deband", plan.deband_active ? "yes" : "no");
    // video-sync / interpolation: ResolveAudioSyncPolicy (bit-perfect coupling + mpv-lazy's vsync_auto rule).
    policy_source_fps_ = source_fps;
    ApplyAudioSyncPolicy(preloading_ ? "preload" : "enhancement", source_fps);
    SetOptionIfChanged("tscale", video_config["tscale"].str("oversample"));
    // HDR output / HDR-source options: ApplyHdrOptions above (also blend-subtitles=no).

    enhancement_plan_ = std::move(plan);
    ApplyVsFailureOverrides();
    video_config_ = video_config;
    shader_failure_recovered_ = false;
    vs_dead_cleanup_pending_ = false; // this pass applied the 7.2c rule itself
    vs_rate_reapply_pending_ = false; // 3A: and the deinterlaced-rate gate
    AppendLog("enhancement applied preset=custom upscale=" + round6_upscale_mode_ + " interpolation=" + round6_interpolation_mode_ + " shaders=" +
              (enhancement_plan_.shader_ids.empty() ? std::string("none") : shader_ids));
    EmitFrameProcessing();
    return true;
}

void MpvBackend::Resize(int, int) {
    // OnResize (main.cpp) already sizes the child; a second SetWindowPos here made every
    // resize, including the fullscreen switch, reconfigure mpv's window twice.
}

std::string MpvBackend::PropertyString(const char* name) {
    if (!ctx_ || !api_ || !initialized_ || !name) return {};
    if (fp_snapshot_active_) { // BuildFrameProcessing: no libmpv call (see EmitFrameProcessing)
        if (const FpSnapshotValue* snap = FpSnapshotFind('S', name)) return snap->ok ? snap->text : std::string();
        if (FpObservedScalar(name)) {
            const auto it = observed_cache_.find(name);
            return it == observed_cache_.end() ? std::string() : it->second.text;
        }
        FpSnapshotMiss('S', name);
    }
    if (!SyncAllowed()) {
        // Observed properties (pause, seeking, idle-active, eof-reached, hwdec-current, sid, aid, ...) from the cache;
        // anything else reads as unknown until the VapourSynth chain is up.
        const auto it = observed_cache_.find(name);
        return it == observed_cache_.end() ? std::string() : it->second.text;
    }
    // Synchronous: only while no VapourSynth chain initialises (or inside on_preloaded, where mpv waits for us).
    char* value = api_->get_property_string(ctx_, name);
    if (!value) return {};
    std::string out(value);
    api_->free(value);
    return out;
}

double MpvBackend::PropertyDouble(const char* name, double fallback) {
    if (!ctx_ || !api_ || !initialized_ || !name) return fallback;
    if (fp_snapshot_active_) { // BuildFrameProcessing: no libmpv call (see EmitFrameProcessing)
        if (const FpSnapshotValue* snap = FpSnapshotFind('D', name)) return snap->ok ? snap->number : fallback;
        if (FpObservedScalar(name)) {
            const auto it = observed_cache_.find(name);
            return it != observed_cache_.end() && it->second.has_number ? it->second.number : fallback;
        }
        FpSnapshotMiss('D', name);
    }
    if (!SyncAllowed()) {
        // time-pos, duration, estimated-vf-fps, container-fps, display-fps, drop counters, avsync: observed.
        const auto it = observed_cache_.find(name);
        return it != observed_cache_.end() && it->second.has_number ? it->second.number : fallback;
    }
    double value = fallback;
    if (GetPropertySync(name, MPV_FORMAT_DOUBLE, &value) < 0) return fallback;
    return value;
}

bool MpvBackend::SetPropertyString(const char* name, const std::string& value) {
    if (!ctx_ || !api_ || !initialized_ || !name) return false;
    if (!SyncAllowed()) return SetPropertyAsync(name, value);
    // Synchronous where the caller needs the result: ApplyEnhancement's structural options (vf, hwdec, scalers, ...)
    // keep applied_options_ in step and detect a rejected amf_frc chain; ConfigureAudio reports options that only take
    // effect on the next file. Never reached while a VapourSynth chain initialises (SyncAllowed above).
    return api_->set_property_string(ctx_, name, value.c_str()) >= 0;
}

bool MpvBackend::SetPropertyAsync(const char* name, const std::string& value) {
    if (!ctx_ || !api_ || !initialized_ || !name) return false;
    const char* data = value.c_str(); // mpv copies the value before set_property_async returns
    const std::uint64_t id = NextReplyId(std::string("set ") + name + "=" + value.substr(0, 200));
    const int result = api_->set_property_async(ctx_, id, name, MPV_FORMAT_STRING, &data);
    if (result < 0) {
        pending_replies_.erase(id);
        AppendLog(std::string("async set rejected: ") + name + "=" + value + " (" + MpvErrorText(api_->error_string, result) + ")");
        return false;
    }
    if (vs_busy_) AppendLog(std::string("vs-busy async set ") + name + "=" + value.substr(0, 200));
    // Keep the cache in step for readers while busy (e.g. Toggle twice before mpv applied the first one).
    const auto cached = observed_cache_.find(name);
    if (cached != observed_cache_.end()) {
        cached->second.text = value;
        char* end = nullptr;
        const double number = value == "yes" ? 1.0 : value == "no" ? 0.0 : std::strtod(value.c_str(), &end);
        cached->second.has_number = value == "yes" || value == "no" || (end && end != value.c_str());
        cached->second.number = cached->second.has_number ? number : 0.0;
    }
    return true;
}

int MpvBackend::SetPropertyNode(const char* name, void* node) {
    if (!ctx_ || !api_ || !initialized_ || !name || !node) return MPV_ERROR_UNINITIALIZED;
    if (SyncAllowed()) // synchronous: ApplyEnhancement reports a rejected shader list with mpv's error text
        return api_->set_property(ctx_, name, MPV_FORMAT_NODE, node);
    const std::uint64_t id = NextReplyId(std::string("set ") + name + " (node)");
    const int result = api_->set_property_async(ctx_, id, name, MPV_FORMAT_NODE, node); // node copied by mpv
    if (result < 0) pending_replies_.erase(id);
    else AppendLog(std::string("vs-busy async set ") + name + " (node)");
    return result;
}

int MpvBackend::GetPropertySync(const char* name, int format, void* data) {
    if (!ctx_ || !api_ || !initialized_ || !name) return MPV_ERROR_UNINITIALIZED;
    if (!SyncAllowed()) return MPV_ERROR_PROPERTY_UNAVAILABLE; // callers already treat < 0 as "unknown"
    if (fp_snapshot_active_ && format == MPV_FORMAT_NODE) FpSnapshotMiss('N', name); // a node read not through ReadNode
    return api_->get_property(ctx_, name, static_cast<mpv_format>(format), data);
}

const mpv_node* MpvBackend::ReadNode(const char* name, NodeHolder& holder) {
    if (fp_snapshot_active_ && name) {
        if (const FpSnapshotValue* snap = FpSnapshotFind('N', name)) return snap->ok && snap->node ? snap->node->get() : nullptr;
        FpSnapshotMiss('N', name);
    }
    if (GetPropertySync(name, MPV_FORMAT_NODE, &holder.owned_) < 0) return nullptr;
    holder.owned_valid_ = true;
    return &holder.owned_;
}

const MpvBackend::FpSnapshotValue* MpvBackend::FpSnapshotFind(char kind, const char* name) const {
    const auto it = fp_snapshot_.find(std::string(1, kind) + name);
    return it == fp_snapshot_.end() ? nullptr : &it->second;
}

void MpvBackend::FpSnapshotMiss(char kind, const char* name) {
    // The tick read something it did not request (a new field): it still works (synchronous read), but add the
    // property to kFpRequests / FpObservedScalar so the UI thread does not wait for mpv again.
    if (fp_snapshot_misses_.insert(std::string(1, kind) + name).second)
        AppendLog(std::string("frameProcessing snapshot miss ") + kind + ":" + name + " (read synchronously)");
}

std::uint64_t MpvBackend::NextReplyId(const std::string& what) {
    const std::uint64_t id = kReplyIdBase + ++reply_seq_;
    pending_replies_[id] = what;
    return id;
}

void MpvBackend::OnAsyncReply(std::uint64_t id, int error, const char* kind) {
    const auto it = pending_replies_.find(id);
    if (it == pending_replies_.end()) return;
    if (error < 0)
        AppendLog(std::string("async ") + kind + " failed: " + it->second + " (" + MpvErrorText(api_ ? api_->error_string : nullptr, error) + ")");
    pending_replies_.erase(it);
}

void MpvBackend::CacheObservedProperty(const void* property_event) {
    const auto* property = static_cast<const mpv_event_property*>(property_event);
    if (!property || !property->name) return;
    if (property->format == MPV_FORMAT_NONE || !property->data) {
        observed_cache_.erase(property->name);
        return;
    }
    CachedProperty value;
    switch (property->format) {
    case MPV_FORMAT_FLAG: {
        const bool flag = *static_cast<int*>(property->data) != 0;
        value.text = flag ? "yes" : "no";
        value.number = flag ? 1.0 : 0.0;
        value.has_number = true;
        break;
    }
    case MPV_FORMAT_DOUBLE:
        value.number = *static_cast<double*>(property->data);
        value.text = std::to_string(value.number); // "%f", as mpv_get_property_string formats a double
        value.has_number = true;
        break;
    case MPV_FORMAT_INT64:
        value.number = static_cast<double>(*static_cast<std::int64_t*>(property->data));
        value.text = std::to_string(*static_cast<std::int64_t*>(property->data));
        value.has_number = true;
        break;
    case MPV_FORMAT_STRING: {
        const char* text = *static_cast<char**>(property->data);
        value.text = text ? text : "";
        char* end = nullptr;
        value.number = std::strtod(value.text.c_str(), &end);
        value.has_number = end && end != value.text.c_str();
        break;
    }
    default:
        return; // node properties (track-list, video-params, ...) are re-read once the chain is up
    }
    observed_cache_[property->name] = std::move(value);
}

bool MpvBackend::VsStageInVf() const {
    const auto vf = applied_options_.find("vf");
    return vf != applied_options_.end() && vf->second.find("vapoursynth") != std::string::npos;
}

// Audit 7.3b. The signature uses the static fields vf_vapoursynth reinitialises on (size, pixel format, colour matrix,
// levels, primaries, transfer); not sig-peak or other per-frame HDR metadata.
void MpvBackend::OnVideoParamsEvent(const void* property_event) {
    const auto* property = static_cast<const mpv_event_property*>(property_event);
    if (!property || property->format != MPV_FORMAT_NODE || !property->data) return; // unavailable: keep the last one
    const auto* node = static_cast<const mpv_node*>(property->data);
    const int w = static_cast<int>(NodeNumber(MapValue(node, "w")));
    const int h = static_cast<int>(NodeNumber(MapValue(node, "h")));
    const std::string pixelformat = NodeString(MapValue(node, "pixelformat"));
    if (w <= 0 || h <= 0 || pixelformat.empty()) return; // incomplete (first report before decoding): not a baseline
    const std::string sig = std::to_string(w) + "x" + std::to_string(h) + " " + pixelformat + " " +
        NodeString(MapValue(node, "colormatrix")) + "/" + NodeString(MapValue(node, "colorlevels")) + "/" +
        NodeString(MapValue(node, "primaries")) + "/" + NodeString(MapValue(node, "gamma"));
    const std::string previous = std::move(reconfig_sig_);
    reconfig_sig_ = sig;
    if (previous.empty() || previous == sig) return;
    // A working VapourSynth stage in vf reloads its script for the new format. Not at the file start (load busy), not
    // while busy (a seek / apply period already covers the reload), not for a chain whose stages all failed (mpv
    // bypasses a disabled filter; nothing reloads).
    if (!file_loaded_ || first_frame_pending_ || vs_busy_ || !VsStageInVf() || AllVsScriptsFailed()) {
        AppendLog("video-params changed " + previous + " -> " + sig + (vs_busy_ ? " (already busy: " + vs_busy_trigger_ + ")" : std::string()));
        return;
    }
    AppendLog("vs-init reconfig: video-params " + previous + " -> " + sig + "; the VapourSynth chain reloads for the new format");
    // Audit 7.3c stays deferred: H_Pre auto is not re-resolved mid-file (that would rebuild the chain: a reload on DML,
    // a new TensorRT engine identity). Logged so a stale H_Pre is visible.
    if (round6_upscale_active_ && round6_upscale_pre_auto_ && h != round6_upscale_pre_source_height_) {
        const int would = AutoUpscalePreHeight(h, round6_upscale_pre_hmax_, round6_upscale_scale_);
        AppendLog("r6-upscale h_pre auto not re-evaluated: mid-stream reconfig src=" + std::to_string(round6_upscale_pre_source_height_) +
                  "->" + std::to_string(h) + " h_pre stays " + std::to_string(round6_upscale_pre_height_) +
                  (would != round6_upscale_pre_height_ ? " (auto would give " + std::to_string(would) + "; audit 7.3c)" : std::string(" (same as auto)")));
    }
    reconfig_watch_probes_ = 0;
    reconfig_watch_max_ms_ = 0.0;
    BeginVsBusy("reconfig");
    if (vs_busy_ && host_ && IsWindow(host_))
        SetTimer(host_, kVsReconfigTimerId, kVsReconfigWatchMs, [](HWND, UINT, UINT_PTR, DWORD) {
            if (g_timer_backend) g_timer_backend->OnVsReconfigTick();
        });
}

void MpvBackend::OnVsReconfigTick() {
    if (!vs_busy_ || vs_busy_trigger_ != "reconfig" || !ctx_ || !api_) {
        if (host_) KillTimer(host_, kVsReconfigTimerId);
        return;
    }
    // One request in flight; none while a seek owns the end or an end is already pending / a barrier runs.
    if (vs_probe_id_ != 0 || vs_restart_pending_ || !vs_end_pending_by_.empty()) return;
    SendVsBusyProbe(4);
}

void MpvBackend::RecheckHdrSource(const char* trigger) {
    if (!file_loaded_ || !SyncAllowed()) return;
    mpv_node params{};
    if (GetPropertySync("video-params", MPV_FORMAT_NODE, &params) < 0) return;
    const std::string source = HdrSourceClass(NodeString(MapValue(&params, "gamma")), NodeNumber(MapValue(&params, "sig-peak")));
    const std::string primaries = NodeString(MapValue(&params, "primaries"));
    api_->free_node_contents(&params);
    if (source != "unknown" && source != hdr_source_) {
        ApplyHdrOptions(trigger, source, primaries);
        enhancement_plan_.hdr_active = IsHdrSourceClass(source);
    }
}

void MpvBackend::BeginVsBusy(const char* trigger) {
    ++fp_epoch_; // a frameProcessing snapshot in flight may straddle the reload: FinishVsBusy requests a fresh one
    if (vs_busy_) {
        // A new reload joins the running busy period. An end that was only pending (met earlier in this drain) or a
        // barrier in flight belonged to the previous reload: drop it, the new reload brings its own end condition.
        // A 7.3b reconfig-watch probe (kind 4) stays: its reply goes through RequestVsBusyEnd, which a joined seek gates.
        const bool cancelled = !vs_end_pending_by_.empty() || vs_probe_kind_ == 2 || vs_probe_kind_ == 3;
        AppendLog(std::string("vs-init busy (trigger=") + vs_busy_trigger_ + "), " + trigger + " joins it" +
                  (cancelled ? " (pending end " + (vs_end_pending_by_.empty() ? vs_barrier_by_ : vs_end_pending_by_) + " cancelled)" : std::string()));
        if (cancelled) {
            vs_end_pending_by_.clear();
            vs_barrier_by_.clear();
            vs_probe_id_ = 0;
            vs_probe_kind_ = 0;
        }
        return;
    }
    vs_busy_ = true;
    vs_busy_trigger_ = trigger;
    ResetRateWindow("vs-busy");
    vs_busy_since_ = std::chrono::steady_clock::now();
    vs_busy_timeout_reported_ = false;
    vs_busy_stop_requested_ = false;
    vs_probe_id_ = 0;
    vs_probe_kind_ = 0;
    vs_barrier_by_.clear();
    vs_end_pending_by_.clear();
    vs_seek_cmds_ = 0;
    vs_seek_events_ = 0;
    vs_restart_pending_ = false;
    if (host_ && IsWindow(host_))
        SetTimer(host_, kVsBusyTimerId, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
            if (g_timer_backend) g_timer_backend->OnVsBusyTick();
        });
    const std::string scripts = !vs_chain_script_.empty() ? vs_chain_script_
        : vs_up_script_ + (vs_up_script_.empty() || vs_fi_script_.empty() ? "" : "+") + vs_fi_script_;
    AppendLog(std::string("vs-init start trigger=") + trigger + " backend=" + VsBusyBackendLog() +
              " scripts=" + (scripts.empty() ? std::string("-") : scripts) + "; libmpv reads use the observed cache, writes are async");
    EmitVsInit("start", "");
}

// The single conditional exit (see the header). by: what met the end condition (playback-restart, probe, ...).
void MpvBackend::RequestVsBusyEnd(const char* by, bool barrier) {
    if (!vs_busy_) return;
    if (vs_restart_pending_) {
        // An executed seek (or refresh seek) is reloading the script: only its PLAYBACK_RESTART may end the period.
        AppendLog(std::string("vs-init end by=") + by + " ignored: a seek is still reloading the chain (seeks=" +
                  std::to_string(vs_seek_events_) + "/" + std::to_string(vs_seek_cmds_) + ")");
        return;
    }
    if (barrier || vs_seek_cmds_ > vs_seek_events_) {
        // Seek commands mpv has not reported yet (queued behind this event, or coalesced): a synchronous read now could
        // wait for the reload they start. Prove the playback thread is past them first, asynchronously.
        if (vs_probe_kind_ == 2 || vs_probe_kind_ == 3) return; // a barrier is already running (a watch probe is replaced)
        vs_end_pending_by_.clear();
        vs_barrier_by_ = by;
        vs_barrier_since_ = std::chrono::steady_clock::now();
        AppendLog(std::string("vs-init end by=") + by + " via probe barrier (seeks=" + std::to_string(vs_seek_events_) + "/" +
                  std::to_string(vs_seek_cmds_) + (barrier ? ", requested" : "") + ")");
        SendVsBusyProbe(2);
        return;
    }
    if (polling_) vs_end_pending_by_ = by; // finished by PollEvents after the drain
    else FinishVsBusy(by);
}

// kind 1: apply readiness (estimated-vf-fps > 0 ends); 2 / 3: barrier steps. One request in flight at a time.
void MpvBackend::SendVsBusyProbe(int kind) {
    if (!ctx_ || !api_ || !vs_busy_) return;
    vs_probe_kind_ = kind;
    vs_probe_id_ = NextReplyId(kind == 1 ? "probe estimated-vf-fps" : kind == 4 ? "reconfig-watch estimated-vf-fps" : "barrier estimated-vf-fps");
    vs_probe_sent_ = std::chrono::steady_clock::now();
    if (api_->get_property_async(ctx_, vs_probe_id_, "estimated-vf-fps", MPV_FORMAT_DOUBLE) >= 0) return;
    pending_replies_.erase(vs_probe_id_);
    vs_probe_id_ = 0;
    vs_probe_kind_ = 0;
    if (kind == 2 || kind == 3) {
        // Cannot queue the barrier (core shutting down): finish as before rather than stay busy forever.
        const std::string by = vs_barrier_by_ + "+barrier-unavailable";
        vs_barrier_by_.clear();
        if (polling_) vs_end_pending_by_ = by;
        else FinishVsBusy(by.c_str());
    }
}

// Unconditional, immediate exit: END_FILE / shutdown (no chain left to wait for).
void MpvBackend::EndVsBusy(const char* by) {
    if (!vs_busy_) return;
    vs_end_pending_by_.clear();
    FinishVsBusy(by);
}

void MpvBackend::FinishVsBusy(const char* by) {
    if (!vs_busy_) return;
    const double elapsed = MsSince(vs_busy_since_);
    const std::string trigger = vs_busy_trigger_;
    vs_busy_ = false;
    ResetRateWindow("vs-busy");
    vs_probe_id_ = 0;
    vs_probe_kind_ = 0;
    vs_barrier_by_.clear();
    vs_end_pending_by_.clear();
    vs_restart_pending_ = false;
    if (host_) KillTimer(host_, kVsBusyTimerId);
    if (host_) KillTimer(host_, kVsReconfigTimerId);
    AppendLog(std::string("vs-init done trigger=") + vs_busy_trigger_ + " by=" + by + " elapsed_ms=" + Number(elapsed, 0) +
              " seeks=" + std::to_string(vs_seek_events_) + "/" + std::to_string(vs_seek_cmds_) +
              (trigger == "reconfig" ? " watch_probes=" + std::to_string(reconfig_watch_probes_) + " longest_wait_ms=" +
                                           Number(reconfig_watch_max_ms_, 0) : std::string()) +
              (vs_busy_timeout_reported_ ? " (after the timeout report)" : ""));
    EmitVsInit("done", by);
    vs_busy_stop_requested_ = false;
    // Work deferred while busy, in the order the events would have done it. Each step may start a new busy period
    // (a changed chain), after which the remaining steps defer themselves again.
    if (vs_deferred_opened_) {
        vs_deferred_opened_ = false;
        if (file_loaded_) EmitOpened();
    }
    if (vs_deferred_subtitle_) {
        vs_deferred_subtitle_ = false;
        if (file_loaded_) EmitSubtitleTrack();
    }
    if (vs_deferred_apply_) {
        vs_deferred_apply_ = false;
        Json config = std::move(vs_deferred_config_);
        vs_deferred_config_ = Json();
        if (vs_deferred_file_enhancement_) video_config_ = config; // the FILE_LOADED pass below applies it
        else if (!applying_round6_enhancement_) {
            applying_round6_enhancement_ = true;
            ApplyEnhancement(config);
            applying_round6_enhancement_ = false;
        }
    }
    if (vs_deferred_file_enhancement_) {
        vs_deferred_file_enhancement_ = false;
        if (file_loaded_) ApplyFileEnhancement(false);
    }
    // Audit 7.3b: the new segment's colour data is readable now (an SDR -> HDR splice changes the VO's HDR options only).
    if (trigger == "reconfig" && file_loaded_ && SyncAllowed() && !applying_round6_enhancement_) RecheckHdrSource("reconfig");
    if (file_loaded_ && SyncAllowed()) {
        last_frame_processing_emit_ = {};
        EmitFrameProcessing();
    }
    // Audit 7.2c: a failure seen during this busy period (load / seek) that the deferred FILE_LOADED pass did not
    // already clean up. Inside the drain (END_FILE, shutdown) PollEvents does it afterwards, if still relevant.
    if (!polling_) RunPendingDeadVsCleanup("busy-end");
    if (!polling_) RunPendingRateReapply("busy-end"); // 3A
}

// What the VapourSynth stages in vf run on, for the vs-init log line: the AI stages (UAI, RIFE, DRBA) on the resolved
// inference backend, MVTools on the CPU (ai_backend_ is not even resolved for it). Same facts as EmitVsInit.
std::string MpvBackend::VsBusyBackendLog() const {
    const bool vs_upscale = round6_upscale_active_ && (round6_upscale_backend_ == "DirectML" || round6_upscale_backend_ == "TensorRT");
    const bool ai_interpolation = round6_interpolation_active_ && (round6_interpolation_mode_ == "rife" || round6_interpolation_mode_ == "drba");
    const bool cpu_interpolation = round6_interpolation_active_ && round6_interpolation_mode_ == "mvtools";
    std::string out;
    if (vs_upscale || ai_interpolation) out = ai_backend_ == "trt" ? "trt" : "dml";
    if (cpu_interpolation) out += out.empty() ? "cpu" : "+cpu";
    return out.empty() ? std::string("none") : out;
}

void MpvBackend::EmitVsInit(const char* state, const std::string& note) {
    const bool trt = ai_backend_ == "trt";
    const bool vs_upscale = round6_upscale_active_ && (round6_upscale_backend_ == "DirectML" || round6_upscale_backend_ == "TensorRT");
    const bool ai_interpolation = round6_interpolation_active_ && (round6_interpolation_mode_ == "rife" || round6_interpolation_mode_ == "drba");
    const std::string backend = vs_upscale || ai_interpolation ? (trt ? "TensorRT" : "DirectML") : "CPU";
    const std::string stage = !vs_chain_script_.empty() || (!vs_up_script_.empty() && !vs_fi_script_.empty()) ? "chain"
        : !vs_up_script_.empty() ? "upscale" : "interpolation";
    Emit("vsInit", std::string("{\"state\":") + Json::Quote(state) + ",\"trigger\":" + Json::Quote(vs_busy_trigger_) +
         ",\"backend\":" + Json::Quote(backend) + ",\"stage\":" + Json::Quote(stage) + ",\"note\":" + Json::Quote(note) +
         ",\"stopping\":" + (vs_busy_stop_requested_ ? "true" : "false") +
         ",\"elapsed_ms\":" + Number(MsSince(vs_busy_since_), 0) + "}");
}

void MpvBackend::OnVsBusyTick() {
    if (!vs_busy_ || !ctx_ || !api_) {
        if (host_) KillTimer(host_, kVsBusyTimerId);
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - vs_busy_since_;
    if (!vs_busy_timeout_reported_ && elapsed >= kVsBusyTimeout) {
        // Reported, not cleared: clearing would let the next synchronous call freeze the window for however long the
        // build still takes. A TensorRT engine build for a large model can legitimately take this long.
        vs_busy_timeout_reported_ = true;
        AppendLog("vs-init timeout trigger=" + vs_busy_trigger_ + " elapsed_ms=" + Number(MsSince(vs_busy_since_), 0) +
                  "; still waiting for mpv's playback thread (state kept busy)");
        EmitVsInit("timeout", "15 min");
    }
    // A chain changed during playback is not followed by MPV_EVENT_PLAYBACK_RESTART (mpv only refresh-seeks while
    // paused). Probe: an asynchronous read is served by mpv's core only once the playback thread left the script load,
    // and estimated-vf-fps > 0 then says frames flow. Sent >= 1 s after the vf change, so the load has started.
    // Not while a seek owns the end (its PLAYBACK_RESTART does), nor while a barrier runs.
    if (vs_busy_trigger_ == "apply" && vs_probe_id_ == 0 && !vs_restart_pending_ && elapsed >= std::chrono::seconds(1))
        SendVsBusyProbe(1);
}

std::string MpvBackend::WasapiDevicesJson() {
    mpv_node value{};
    if (!ctx_ || !api_ || !initialized_) return "[]";
    if (!SyncAllowed()) return wasapi_devices_cache_; // last list; refreshed on the next call after the chain is up
    if (GetPropertySync("audio-device-list", MPV_FORMAT_NODE, &value) < 0) return "[]";
    std::string json = "[";
    bool first = true;
    if (value.format == MPV_FORMAT_NODE_ARRAY && value.u.list) {
        for (int i = 0; i < value.u.list->num; ++i) {
            const mpv_node* item = &value.u.list->values[i];
            const std::string name = NodeString(MapValue(item, "name"));
            if (name.rfind("wasapi/", 0) != 0) continue;
            if (!first) json += ',';
            first = false;
            json += "{\"id\":" + Json::Quote(name) + ",\"name\":" +
                Json::Quote(NodeString(MapValue(item, "description"), name.c_str())) + "}";
        }
    }
    api_->free_node_contents(&value);
    wasapi_devices_cache_ = json + ']';
    return wasapi_devices_cache_;
}

std::string MpvBackend::AudioStatusJson() const {
    return "\"audioHost\":\"engine\",\"audioEngine\":\"wasapi\"";
}

void MpvBackend::HandleLogLine(const std::string& prefix, const std::string& level, const std::string& line) {
    // "[vapoursynth] fatal: Script evaluation failed:" — mpv's module prefix and level, like mpv.log's [prefix][l].
    AppendLog(prefix.empty() ? line : "[" + prefix + "] " + level + ": " + line);
    if (line.find("first video frame after restart shown") != std::string::npos) {
        RecordShaderCompileMeasurement();
        // Only delivered when the log level is verbose; PLAYBACK_RESTART is the usual trigger.
        if (first_frame_pending_ && file_loaded_) PresentFirstFrame("log");
    }
    RecoverShaderCompileFailure(line);
    HandleVapourSynthFailureLine(prefix, line);
    const std::string lower = Lower(line);
    if (prefix.rfind("ao", 0) == 0 && (level == "error" || level == "fatal" || level == "warn")) last_ao_error_ = line;
    if (lower.find("could not open/initialize audio device") != std::string::npos) OnAudioOutputFailure(line);
    // mpv replaces a user filter that fails at runtime with a pass-through ("Disabling filter amf_frc.00 because it
    // has failed."), e.g. when AMF cannot initialize or receives non-D3D11 frames. Report it instead of claiming 2x.
    if (round6_interpolation_active_ && round6_interpolation_mode_ == "amf_frc" &&
        lower.find("disabling filter amf_frc") != std::string::npos) {
        round6_interpolation_active_ = false;
        round6_interpolation_factor_ = 0.0;
        round6_interpolation_fps_num_ = 1;
        round6_interpolation_fps_den_ = 1;
        round6_interpolation_reason_ = "amf_failed";
        AppendLog("r6-interpolation amf_frc disabled by mpv at runtime; playback continues at the source rate");
        EmitError("INTERPOLATION_UNAVAILABLE", "AMD FRC 補幀無法啟用，已改回原始幀率播放。", false);
        last_frame_processing_emit_ = {};
        EmitFrameProcessing();
    }
    if (lower.find("device lost") != std::string::npos || lower.find("device hung") != std::string::npos ||
        lower.find("dxgi_error_device_hung") != std::string::npos || lower.find("device_removed") != std::string::npos ||
        lower.find("device removed") != std::string::npos || lower.find("dxgi_error_device_removed") != std::string::npos)
        RecoverDeviceLoss();
}

// mpv could not open the AO ("Could not open/initialize audio device -> no sound.") and dropped the audio track.
// Within 10 s of an OFF->ON switch this is the exclusive open failing (device held by another application, format
// refused): revert to shared mode at the saved level, re-select the audio track, and tell the UI to revert the
// checkbox. Otherwise (a file opened in exclusive mode) only report it.
void MpvBackend::OnAudioOutputFailure(const std::string& line) {
    const bool exclusive = audio_exclusive_ == "yes";
    const bool transition = exclusive && exclusive_requested_at_.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - exclusive_requested_at_ < std::chrono::seconds(10);
    exclusive_requested_at_ = {};
    const std::string detail = last_ao_error_.empty() ? line : last_ao_error_;
    AppendLog("audio output failed exclusive=" + audio_exclusive_ + " transition=" + (transition ? "yes" : "no") + " detail=" + detail);
    if (!exclusive) {
        EmitError("AUDIO_OUTPUT_FAILED", "無法開啟音訊輸出：" + detail, false);
        return;
    }
    if (transition) {
        audio_exclusive_ = "no";
        AppendLog("audio-transition revert on->off step=1 volume=" + std::to_string(audio_volume_) + " (saved)");
        SetPropertyAsync("volume", std::to_string(audio_volume_));
        AppendLog("audio-transition revert on->off step=2 audio-exclusive=no");
        SetPropertyAsync("audio-exclusive", "no");
        SetOptionIfChanged("audio-pitch-correction", "yes");
        ApplyAudioSyncPolicy("exclusive-failed");
        SetPropertyAsync("mute", audio_mute_ ? "yes" : "no");
        if (!last_aid_.empty()) {
            AppendLog("audio-transition revert step=3 aid=" + last_aid_ + " (re-select after the AO failure)");
            SetPropertyAsync("aid", last_aid_);
        }
    }
    Emit("audioPolicy", "{\"event\":\"exclusive_failed\",\"reverted\":" + std::string(transition ? "true" : "false") +
         ",\"message\":" + Json::Quote(detail) + "}");
}

namespace {
bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string TrimmedLine(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// First line of an error message, at most ~200 bytes, cut on a UTF-8 character boundary (k7sfunc errors are Chinese).
std::string FirstErrorLine(const std::string& text) {
    std::string line = TrimmedLine(text.substr(0, text.find('\n')));
    constexpr std::size_t kMax = 200;
    if (line.size() > kMax) {
        std::size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        line = line.substr(0, cut) + "…";
    }
    return line;
}
} // namespace

// mpv logs a failing VapourSynth stage like this (strings from this libmpv build; levels f = fatal, e = error):
//   [vapoursynth] f: Script evaluation failed:            <- the script raised while being evaluated (load or seek)
//   [vapoursynth] f: Python exception: <message>          <- first line we show (k7sfunc: 模块 … 所请求的模型缺失)
//   [vapoursynth] f: Traceback (most recent call last):
//   [vapoursynth] f:   File "…/data/cache/vs/up_<hash>.vpy", line N, in <module>   <- maps the failure to a stage
//   [vapoursynth] e: could not init VS
//   [vf] e: Disabling filter vapoursynth.NN because it has failed.                  <- mpv bypasses the filter
// A runtime error (e.g. a TensorRT exception while filtering) is "Filter error at frame N: <message>"; a broken
// runtime is "Failed to load/initialize VapourSynth VSScript library: …". Both also end in "Disabling filter".
void MpvBackend::HandleVapourSynthFailureLine(const std::string& prefix, const std::string& line) {
    const std::string text = TrimmedLine(line);
    if (text.empty()) return;
    const bool vs_prefix = prefix.empty() || prefix.find("vapoursynth") != std::string::npos;
    if (vs_prefix) {
        if (StartsWith(text, "Script evaluation failed")) {
            vs_capture_active_ = true;
            vs_capture_stage_.clear();
            vs_capture_detail_.clear();
            vs_capture_source_ = "script";
            return;
        }
        if (StartsWith(text, "Failed to load VapourSynth VSScript library") ||
            StartsWith(text, "Failed to initialize VapourSynth VSScript library") ||
            StartsWith(text, "Could not get vapoursynth API handle")) {
            vs_capture_active_ = true;
            vs_capture_stage_ = "all";
            vs_capture_detail_ = text;
            vs_capture_source_ = "runtime";
            return;
        }
        if (StartsWith(text, "Filter error at frame")) {
            // The message may start on the next line (a Python traceback ending in "vapoursynth.Error: …").
            const auto colon = text.find(':');
            vs_capture_active_ = true;
            vs_capture_stage_.clear();
            vs_capture_detail_ = colon == std::string::npos ? std::string() : TrimmedLine(text.substr(colon + 1));
            vs_capture_source_ = "frame";
            return;
        }
        if (vs_capture_active_) {
            if (vs_capture_detail_.empty()) {
                if (StartsWith(text, "Python exception: ")) vs_capture_detail_ = text.substr(18);
                else if (StartsWith(text, "vapoursynth.Error: ")) vs_capture_detail_ = text.substr(19);
                // k7sfunc's vs.Error text (模块 … 所请求的模型缺失), but not the traceback's source line that raises it.
                else if (text.find("模块 ") != std::string::npos && !StartsWith(text, "raise") && text.find('{') == std::string::npos)
                    vs_capture_detail_ = text;
            }
            // The traceback names the generated script that raised (chain_<hash>.vpy holds both stages).
            if (vs_capture_stage_.empty() && text.find(".vpy") != std::string::npos) {
                if (!vs_chain_script_.empty() && text.find(vs_chain_script_) != std::string::npos) vs_capture_stage_ = "chain";
                else if (!vs_up_script_.empty() && text.find(vs_up_script_) != std::string::npos) vs_capture_stage_ = "upscale";
                else if (!vs_fi_script_.empty() && text.find(vs_fi_script_) != std::string::npos) vs_capture_stage_ = "interpolation";
            }
        }
    }
    const auto disabling = text.find("Disabling filter vapoursynth");
    if (disabling == std::string::npos) return;
    std::string stage = vs_capture_active_ ? vs_capture_stage_ : std::string();
    std::string detail = vs_capture_active_ ? vs_capture_detail_ : std::string();
    const std::string source = vs_capture_active_ ? vs_capture_source_ : std::string("filter");
    vs_capture_active_ = false;
    vs_capture_stage_.clear();
    vs_capture_detail_.clear();
    vs_capture_source_.clear();
    if (stage.empty()) {
        // No traceback path (frame error): one VS stage in the chain is unambiguous; with two, mpv's filter name
        // vapoursynth.NN gives the position in the vf list (vs_chain_order_). A chain script is the only vapoursynth
        // filter, so vapoursynth.00 (or any VS failure) means the whole chain.
        const bool up = !vs_up_script_.empty();
        const bool fi = !vs_fi_script_.empty();
        if (!vs_chain_script_.empty()) stage = "chain";
        else if (up && !fi) stage = "upscale";
        else if (fi && !up) stage = "interpolation";
        else if (up && fi) {
            const std::string name_start = "Disabling filter vapoursynth.";
            const auto digits = disabling + name_start.size();
            int index = -1;
            if (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits])))
                index = std::atoi(text.c_str() + digits);
            if (index >= 0 && static_cast<std::size_t>(index) < vs_chain_order_.size()) stage = vs_chain_order_[index];
        }
    }
    if (detail.empty()) detail = text;
    if (stage == "all" && !vs_chain_script_.empty()) {
        MarkVsStageFailed("chain", detail, source);
    } else if (stage == "all") {
        if (!vs_up_script_.empty()) MarkVsStageFailed("upscale", detail, source);
        if (!vs_fi_script_.empty()) MarkVsStageFailed("interpolation", detail, source);
    } else if (stage == "upscale" || stage == "interpolation" || stage == "chain") {
        MarkVsStageFailed(stage, detail, source);
    } else {
        AppendLog("vs-failure could not be mapped to a stage (source=" + source + "): " + FirstErrorLine(detail));
    }
}

// stage: upscale | interpolation | chain (one script with both stages: both are marked failed, reason vs_chain_failed).
void MpvBackend::MarkVsStageFailed(const std::string& stage, const std::string& detail, const std::string& source) {
    const bool chain = stage == "chain";
    const bool upscale = stage == "upscale";
    const std::string& script = chain ? vs_chain_script_ : upscale ? vs_up_script_ : vs_fi_script_;
    if (script.empty()) return;
    const std::string first = FirstErrorLine(detail);
    // 3A: k7sfunc FPS_CTRL refused the rate the script received although the container-fps gate passed (mpv's auto
    // deinterlacer doubles it at the first frame, before the host can know). The late form of the 7.2a refusal: no
    // "script failed" toast; the chain is recomposed once without interpolation (RunPendingRateReapply).
    const bool fps_limit = (chain || stage == "interpolation") && first.find("源帧率超过限制") != std::string::npos;
    if (fps_limit) {
        vs_fps_limit_seen_ = true;
        vs_rate_reapply_pending_ = true;
    }
    const bool was_active = chain ? (round6_upscale_active_ || round6_interpolation_active_)
        : upscale ? round6_upscale_active_ : round6_interpolation_active_;
    vs_failed_scripts_.insert(script);
    if (chain || upscale) vs_upscale_error_ = first;
    if (chain || !upscale) vs_interpolation_error_ = first;
    ApplyVsFailureOverrides();
    AppendLog("vs-failure stage=" + stage + " source=" + source + " script=" + script + " was_active=" +
              (was_active ? "yes" : "no") + " error=" + first +
              (fps_limit ? " classified=fps-limit (script input above 32 fps while the container-fps gate passed; deinterlace-active=" +
                               std::string(deint_seen_ ? "yes" : "not yet") + "): recomposition pending, no failure toast" : std::string()));
    // Audit 7.2c: with no working VapourSynth stage left, the dead filter (and the auto-copy readback it needs) is removed
    // from the event loop (RunPendingDeadVsCleanup), never from here: this runs inside mpv's log-message handling.
    const bool remove_dead = AllVsScriptsFailed() && VsStageInVf() && vs_dead_cleanups_ < kVsDeadCleanupMax;
    if (remove_dead && !vs_dead_cleanup_pending_) {
        vs_dead_cleanup_pending_ = true;
        AppendLog("vs-dead-cleanup pending: no working VapourSynth stage left in vf (cleanups so far " +
                  std::to_string(vs_dead_cleanups_) + "/" + std::to_string(kVsDeadCleanupMax) + ")");
    }
    const std::string removed = remove_dead
        ? "，已停用該濾鏡並改回原生解碼（" + ResolveHwdec(gpu_api_, video_config_["hwdec"].str("d3d11va")) + "）" : std::string();
    if (!fps_limit && vs_failure_notified_.insert(stage + "|" + first).second) {
        if (chain) EmitError("VS_CHAIN_FAILED", "AI 放大＋補幀腳本失敗" + removed + "：" + first, false);
        else if (upscale) EmitError("VS_UPSCALE_FAILED", "AI 放大腳本失敗" + removed + "：" + first, false);
        else EmitError("VS_INTERPOLATION_FAILED", std::string(round6_interpolation_mode_ == "mvtools" ? "補幀腳本失敗" : "AI 補幀腳本失敗") + removed + "：" + first, false);
    }
    last_frame_processing_emit_ = {};
    EmitFrameProcessing();
}

// A stage whose generated script failed for this file stays off the panel even when ApplyEnhancement runs again
// with the same script (FILE_LOADED pass, fps change): mpv keeps that filter bypassed until the chain is rebuilt.
void MpvBackend::ApplyVsFailureOverrides() {
    const bool chain_failed = !vs_chain_script_.empty() && vs_failed_scripts_.count(vs_chain_script_);
    if (!vs_up_script_.empty() && vs_failed_scripts_.count(vs_up_script_)) {
        round6_upscale_active_ = false;
        round6_upscale_backend_.clear();
        round6_upscale_reason_ = chain_failed ? "vs_chain_failed" : "vs_upscale_failed";
        enhancement_plan_.ai_dml = false;
        enhancement_plan_.upscale = "off";
        enhancement_plan_.upscale_scale = 1;
    }
    if (!vs_fi_script_.empty() && vs_failed_scripts_.count(vs_fi_script_)) {
        round6_interpolation_active_ = false;
        round6_interpolation_factor_ = 0.0;
        round6_interpolation_fps_num_ = 1;
        round6_interpolation_fps_den_ = 1;
        round6_interpolation_reason_ = chain_failed ? "vs_chain_failed" : "vs_interpolation_failed";
    }
}

bool MpvBackend::AllVsScriptsFailed() const {
    if (!vs_chain_script_.empty()) return vs_failed_scripts_.count(vs_chain_script_) != 0;
    if (vs_up_script_.empty() && vs_fi_script_.empty()) return false;
    return (vs_up_script_.empty() || vs_failed_scripts_.count(vs_up_script_)) &&
           (vs_fi_script_.empty() || vs_failed_scripts_.count(vs_fi_script_));
}

void MpvBackend::RunPendingDeadVsCleanup(const char* where) {
    if (!vs_dead_cleanup_pending_) return;
    // Kept pending (retried after the next drain / busy end) while the core may be busy or another pass is running.
    // Device-loss recovery reloads the file (START_FILE clears this state); a LAN stream never builds VS stages.
    if (!ctx_ || !api_ || !initialized_ || !file_loaded_ || vs_busy_ || polling_ || !SyncAllowed() ||
        applying_round6_enhancement_ || handling_device_loss_ || device_loss_original_pending_ || lan_stream_) return;
    vs_dead_cleanup_pending_ = false;
    if (!VsStageInVf() || !AllVsScriptsFailed()) {
        AppendLog(std::string("vs-dead-cleanup not needed (") + where + "): " +
                  (!VsStageInVf() ? "no VapourSynth filter in vf" : "a working VapourSynth stage is in vf"));
        return;
    }
    if (vs_dead_cleanups_ >= kVsDeadCleanupMax) {
        AppendLog("vs-dead-cleanup skipped: limit of " + std::to_string(kVsDeadCleanupMax) + " per file reached");
        return;
    }
    ++vs_dead_cleanups_;
    AppendLog(std::string("vs-dead-cleanup run (") + where + ", " + std::to_string(vs_dead_cleanups_) + "/" +
              std::to_string(kVsDeadCleanupMax) + "): re-applying the current settings without the failed scripts");
    const auto started = std::chrono::steady_clock::now();
    applying_round6_enhancement_ = true;
    const bool applied = ApplyEnhancement(video_config_);
    applying_round6_enhancement_ = false;
    AppendLog("vs-dead-cleanup " + std::string(applied ? "done" : "failed") + " in " + Number(MsSince(started), 1) +
              " ms (UI thread; hwdec then vf, synchronous)");
}

// 3A: recompose the chain once the deinterlaced input rate is known (deinterlace-active, or FPS_CTRL's refusal). Same
// call points and guards as RunPendingDeadVsCleanup; a full ApplyEnhancement pass clears the flag, so the deferred
// FILE_LOADED pass at the end of the load busy period normally consumes it first.
void MpvBackend::RunPendingRateReapply(const char* where) {
    if (!vs_rate_reapply_pending_) return;
    if (!ctx_ || !api_ || !initialized_ || !file_loaded_ || vs_busy_ || polling_ || !SyncAllowed() ||
        applying_round6_enhancement_ || handling_device_loss_ || device_loss_original_pending_ || lan_stream_) return;
    vs_rate_reapply_pending_ = false;
    AppendLog(std::string("vs-rate-reapply run (") + where + "): deinterlace-active=" + (deint_seen_ ? "yes" : "no") +
              " fps-limit-seen=" + (vs_fps_limit_seen_ ? "yes" : "no") + "; re-applying the current settings at the script's input rate");
    const auto started = std::chrono::steady_clock::now();
    applying_round6_enhancement_ = true;
    const bool applied = ApplyEnhancement(video_config_);
    applying_round6_enhancement_ = false;
    AppendLog("vs-rate-reapply " + std::string(applied ? "done" : "failed") + " in " + Number(MsSince(started), 1) + " ms");
}

void MpvBackend::ResetRateWindow(const char* why) {
    rate_samples_.clear();
    rate_reset_reason_ = why ? why : "";
    rate_window_s_ = -1.0;
    rate_delivered_fps_ = -1.0;
    rate_drops_delta_ = -1.0;
    rate_avsync_slope_ = 0.0;
    sync_drops_per_s_ = -1.0;
    sync_presented_fps_ = -1.0;
}

bool MpvBackend::ObservedFlag(const char* name) const {
    const auto it = observed_cache_.find(name);
    return it != observed_cache_.end() && it->second.text == "yes";
}

void MpvBackend::ResetPerFileDiagnostics() {
    trt_fallback_notified_ = false; // the TensorRT -> DirectML fallback toast is shown once per file
    vs_failed_scripts_.clear();
    vs_failure_notified_.clear();
    vs_capture_active_ = false;
    vs_capture_stage_.clear();
    vs_capture_detail_.clear();
    vs_capture_source_.clear();
    vs_upscale_error_.clear();
    vs_interpolation_error_.clear();
    vs_dead_cleanup_pending_ = false;
    vs_dead_cleanups_ = 0;
    vs_dead_removed_ = false;
    reconfig_sig_.clear(); // 7.3b: the new file's first complete video-params is its baseline
    deint_seen_ = false;   // 3A: per file
    vs_fps_limit_seen_ = false;
    vs_rate_reapply_pending_ = false;
    deint_active_logged_.clear();
    ResetRateWindow("file");
    ++fp_epoch_; // a frameProcessing snapshot in flight belongs to the previous file
    sync_trace_at_ = {};
    hdr_ai_note_logged_ = false;  // the HDR + AI parity note is logged once per file
    display_caps_dirty_ = true;   // re-read the display HDR state for each file
}

void MpvBackend::RecoverShaderCompileFailure(const std::string& line) {
    if (!ctx_ || !api_ || !initialized_ || shader_failure_recovered_ || enhancement_plan_.shader_paths.empty()) return;
    const std::string lower = Lower(line);
    const bool compile_error = lower.find("shader") != std::string::npos &&
        (lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos ||
         lower.find("failure") != std::string::npos);
    if (!compile_error) return;

    std::size_t failed_path = std::string::npos;
    for (std::size_t i = 0; i < enhancement_plan_.shader_paths.size(); ++i) {
        const std::filesystem::path path = std::filesystem::u8path(enhancement_plan_.shader_paths[i]);
        const std::string filename = Lower(path.filename().string());
        if (!filename.empty() && lower.find(filename) != std::string::npos) {
            failed_path = i;
            break;
        }
    }
    if (failed_path == std::string::npos) {
        // A single-shader chain is unambiguous even if mpv omits the source filename.
        if (enhancement_plan_.shader_ids.size() == 1) failed_path = 0;
        else {
            AppendLog("shader compile failure observed; source could not be mapped, leaving chain unchanged");
            return;
        }
    }

    const std::size_t failed_id = enhancement_plan_.shader_ids.size() == enhancement_plan_.shader_paths.size()
        ? failed_path : (enhancement_plan_.shader_ids.size() == 1 ? 0 : std::string::npos);
    if (failed_id == std::string::npos) {
        AppendLog("shader compile failure observed; source-to-stage mapping is ambiguous, leaving chain unchanged");
        return;
    }
    const std::string id = enhancement_plan_.shader_ids[failed_id];
    if (enhancement_plan_.restore == id) enhancement_plan_.restore = "off";
    if (enhancement_plan_.upscale == id) enhancement_plan_.upscale = "off";
    if (enhancement_plan_.sharpen == id) enhancement_plan_.sharpen = "off";
    enhancement_plan_.shader_ids.erase(enhancement_plan_.shader_ids.begin() + static_cast<std::ptrdiff_t>(failed_id));
    enhancement_plan_.shader_paths.erase(enhancement_plan_.shader_paths.begin() + static_cast<std::ptrdiff_t>(failed_path));

    std::vector<mpv_node> shader_nodes(enhancement_plan_.shader_paths.size());
    for (std::size_t i = 0; i < shader_nodes.size(); ++i) {
        shader_nodes[i].format = MPV_FORMAT_STRING;
        shader_nodes[i].u.string = const_cast<char*>(enhancement_plan_.shader_paths[i].c_str());
    }
    mpv_node_list shader_list{};
    shader_list.num = static_cast<int>(shader_nodes.size());
    shader_list.values = shader_nodes.empty() ? nullptr : shader_nodes.data();
    mpv_node shader_value{};
    shader_value.format = MPV_FORMAT_NODE_ARRAY;
    shader_value.u.list = &shader_list;
    if (shader_nodes.empty()) SetPropertyString("glsl-shaders", "");
    else SetPropertyNode("glsl-shaders", &shader_value);
    enhancement_plan_.active = !enhancement_plan_.shader_paths.empty() || !enhancement_plan_.filter.empty() ||
        enhancement_plan_.deband_active || enhancement_plan_.smooth_active || enhancement_plan_.hdr_active;
    enhancement_plan_.reason = "shader_compile_error";
    shader_failure_recovered_ = true;
    applied_options_.erase("glsl-shaders");
    AppendLog("shader compile failure; removed=" + id + "; playback continues");
    EmitError("SHADER_COMPILE_FAILED", "著色器編譯失敗，已移除該項，影片會繼續播放。", false);
    last_frame_processing_emit_ = {};
    EmitFrameProcessing();
}

void MpvBackend::RecoverDeviceLoss() {
    if (!ctx_ || !initialized_ || handling_device_loss_ || !file_loaded_ || current_path_.empty()) return;
    handling_device_loss_ = true;
    if (device_loss_retries_ > 0) {
        AppendLog("GPU device loss repeated for this file; stopping without another retry");
        Stop();
        EmitError("GPU_DEVICE_LOST", "GPU 裝置再次中斷，已停止播放。請關閉播放器並檢查顯示驅動記錄。", true);
        handling_device_loss_ = false;
        return;
    }
    ++device_loss_retries_;
    const std::string path = current_path_;
    const std::int64_t position = PositionMs();
    AppendLog("GPU device lost; retrying current file once from " + std::to_string(position) + " ms with the original preset");
    file_loaded_ = false;
    // Surface handshake: the lost device's image is not trustworthy, so the child is hidden now and
    // shown again at the reload's first frame (PresentFirstFrame). pending_open_ makes the END_FILE of
    // the stop below a switch (state "opening", not "stopped") and keeps MediaActive() true, so the host
    // erases black instead of the ground colour meanwhile.
    pending_open_ = true;
    first_frame_pending_ = false;
    stop_requested_ = false;
    open_requested_at_ = std::chrono::steady_clock::now();
    open_start_ms_ = std::max<std::int64_t>(0, position);
    if (host_) KillTimer(host_, kFirstFrameTimerId);
    if (video_child_ && IsWindow(video_child_)) ShowWindow(video_child_, SW_HIDE);
    Command({"stop"});
    SetPropertyString("glsl-shaders", "");
    Command({"vf", "clr", ""});
    SetPropertyString("hwdec", base_hwdec_);
    applied_options_.clear();
    const std::string start = "start=" + Number(position / 1000.0, 3);
    std::string per_file_options = start + ",pause=no";
    if (!lan_stream_) per_file_options += LocalSubFontsDirOption(path);
    const char* args[] = {"loadfile", path.c_str(), "replace", "-1", per_file_options.c_str(), nullptr};
    device_loss_original_pending_ = true;
    const std::uint64_t reload_id = NextReplyId("loadfile (device-loss retry)");
    BeginLoadRequest(reload_id, "device-loss"); // 2B: the reload is a load request too (its rejection is reported)
    const int reload_result = api_->command_async(ctx_, reload_id, args);
    current_path_ = path;
    if (reload_result < 0) {
        pending_replies_.erase(reload_id);
        load_replies_.erase(reload_id);
        FailLoad("GPU_DEVICE_LOST", "GPU 裝置中斷後無法重新開啟檔案：" + LoadErrorText(reload_result), reload_result, "submit", false);
        handling_device_loss_ = false;
        return;
    }
    EmitError("GPU_DEVICE_LOST", "GPU 裝置短暫中斷，已從目前位置以原始預設重新開啟。", false);
    handling_device_loss_ = false;
}

#if defined(VP_TEST_HARNESS)
void MpvBackend::DebugInjectLogLine(const std::string& line) {
    HandleLogLine("", "", line);
}
#endif

void MpvBackend::AppendLog(const std::string& line) {
    if (host_log_path_.empty()) return;
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_s(&local, &time);
    char timestamp[32]{};
    std::snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03lld ",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<long long>(millis));
    std::string entry(timestamp);
    entry += line;
    if (entry.empty() || entry.back() != '\n') entry.push_back('\n');
    {
        std::lock_guard lock(log_mutex_);
        if (!log_writer_started_ || log_writer_stop_) return;
        log_queue_.push_back(std::move(entry));
    }
    log_cv_.notify_one();
}

bool MpvBackend::StartLogWriter() {
    std::lock_guard lock(log_mutex_);
    if (log_writer_started_) return true;
    log_writer_stop_ = false;
    try {
        log_thread_ = std::thread(&MpvBackend::LogWriterMain, this);
    } catch (...) {
        return false;
    }
    log_writer_started_ = true;
    return true;
}

void MpvBackend::LogWriterMain() {
    std::ofstream out(host_log_path_, std::ios::binary | std::ios::app);
    for (;;) {
        std::deque<std::string> batch;
        bool stop = false;
        {
            std::unique_lock lock(log_mutex_);
            log_cv_.wait(lock, [this] { return log_writer_stop_ || !log_queue_.empty(); });
            if (!log_writer_stop_ && log_queue_.size() < 256)
                log_cv_.wait_for(lock, std::chrono::milliseconds(25), [this] {
                    return log_writer_stop_ || log_queue_.size() >= 256;
                });
            batch.swap(log_queue_);
            stop = log_writer_stop_;
        }
        if (out) {
            for (const auto& line : batch) out.write(line.data(), static_cast<std::streamsize>(line.size()));
            out.flush();
        }
        if (stop && batch.empty()) break;
    }
}

void MpvBackend::StopLogWriter() {
    {
        std::lock_guard lock(log_mutex_);
        if (!log_writer_started_) return;
        log_writer_stop_ = true;
    }
    log_cv_.notify_all();
    if (log_thread_.joinable()) log_thread_.join();
    std::lock_guard lock(log_mutex_);
    log_writer_started_ = false;
    log_queue_.clear();
}

// App exit, step 1 (WM_DESTROY): stop talking to libmpv and start mpv_terminate_destroy on a worker. It queues quit
// and joins mpv's core thread, which cannot finish while the playback thread is inside a VapourSynth script load (a
// TensorRT engine build takes minutes), so nothing here waits for it. The worker holds only the function pointer and
// the handle; libmpv-2.dll is never unloaded, so nothing it uses goes away underneath it.
void MpvBackend::BeginShutdown() {
    if (host_) {
        KillTimer(host_, kVsBusyTimerId);
        KillTimer(host_, kVsReconfigTimerId);
        KillTimer(host_, kFirstFrameTimerId);
    }
    // --- core-reliability 1B (wake retry) begin: stop wakes; remove the callback; then cancel+close the retry timer ---
    wake_stopping_.store(true, std::memory_order_release);
    if (!ctx_) { CloseWakeRetry(); return; }
    if (initialized_ && api_ && api_->set_wakeup_callback) api_->set_wakeup_callback(ctx_, nullptr, nullptr);
    CloseWakeRetry(); // mpv's setter waited for an in-flight callback; nothing re-arms the timer from here
    // --- core-reliability 1B end ---
    mpv_handle* const ctx = ctx_;
    ctx_ = nullptr; // Ready() is false from here: no other call reaches libmpv
    if (!api_ || !api_->terminate_destroy) return;
    const auto destroy = api_->terminate_destroy;
    shutdown_started_ = std::chrono::steady_clock::now();
    if (vs_busy_)
        AppendLog("shutdown during VapourSynth init (trigger=" + vs_busy_trigger_ + " elapsed_ms=" +
                  Number(MsSince(vs_busy_since_), 0) + "): quit is queued behind the filter");
    shutdown_done_ = std::shared_ptr<void>(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* handle) {
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
    });
    if (shutdown_done_.get()) {
        try {
            std::thread([destroy, ctx, done = shutdown_done_] {
#if defined(VP_FAULT_INJECTION)
                char delay[16]{}; // core-reliability 2A test only: a terminate_destroy slower than the 10 s bound
                if (GetEnvironmentVariableA("VP_TEST_SHUTDOWN_DELAY_MS", delay, sizeof delay) > 0) Sleep(static_cast<DWORD>(std::atoi(delay)));
#endif
                destroy(ctx);
                SetEvent(static_cast<HANDLE>(done.get()));
            }).detach();
            return;
        } catch (...) {
        }
    }
    shutdown_done_.reset();
    destroy(ctx); // no event or thread: wait here, as before
    AppendLog("libmpv shutdown ms=" + Number(MsSince(shutdown_started_), 0) + " (inline)");
}

// App exit, step 2 (after the message loop, no window left) and the destructor: wait for the worker at most
// kShutdownWaitMs, then leave it to process exit, like the trtexec child, which finishes its engine on its own.
bool MpvBackend::AwaitShutdown(DWORD ms) {
    return !shutdown_done_ || WaitForSingleObject(static_cast<HANDLE>(shutdown_done_.get()), ms) == WAIT_OBJECT_0;
}

void MpvBackend::Shutdown() {
    BeginShutdown();
    if (shutdown_done_) {
        // kShutdownWaitMs counts from BeginShutdown (WM_DESTROY may already have waited part of it).
        const double spent = MsSince(shutdown_started_);
        const DWORD left = spent >= kShutdownWaitMs ? 0 : kShutdownWaitMs - static_cast<DWORD>(spent);
        const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(shutdown_done_.get()), left);
        if (waited == WAIT_OBJECT_0) {
            AppendLog("libmpv shutdown ms=" + Number(MsSince(shutdown_started_), 0));
        } else {
            shutdown_detached_ = true;
            AppendLog("libmpv shutdown did not finish within " + std::to_string(kShutdownWaitMs) +
                      " ms (VapourSynth init still running on mpv's playback thread); leaving it to process exit");
        }
        shutdown_done_.reset();
    }
    initialized_ = false;
    api_.reset();
    StopLogWriter();
    file_loaded_ = false;
    if (g_timer_backend == this) g_timer_backend = nullptr;
    host_ = nullptr;
    video_child_ = nullptr;
}

} // namespace vp
