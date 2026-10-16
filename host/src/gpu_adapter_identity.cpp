#include "gpu_adapter_identity.h"

#include "mpv_config_safety.h"

#include <windows.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>

namespace vp::gpu {
namespace {

std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// mpv bstr_case_startswith: ASCII case-insensitive prefix test on the UTF-8 bytes.
bool CaseStartsWith(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (AsciiLower(text[i]) != AsciiLower(prefix[i])) return false;
    return true;
}

bool KeyMatches(const SavedSelection& saved, const AdapterInfo& adapter) {
    return saved.vendor == adapter.vendor && saved.device == adapter.device && saved.subsys == adapter.subsys &&
           saved.revision == adapter.revision;
}

std::string Hex(std::uint32_t value, int width) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof buffer, "0x%0*X", width, value);
    return buffer;
}

bool NumberMember(const Json& object, const char* key, std::uint32_t* out) {
    const Json& value = object[key];
    if (value.type() != Json::Type::Number || value.num() < 0 || value.num() > 4294967295.0) return false;
    *out = static_cast<std::uint32_t>(value.num());
    return true;
}

std::string LuidText(const std::vector<AdapterInfo>& adapters, int index) {
    return index >= 0 && index < static_cast<int>(adapters.size()) ? std::to_string(adapters[index].luid) : std::string();
}

std::string NameOf(const std::vector<AdapterInfo>& adapters, int index) {
    return index >= 0 && index < static_cast<int>(adapters.size()) ? adapters[index].name : std::string();
}

// Index of the closing quote of the JSON string opening at text[open] (npos if unterminated).
std::size_t StringEnd(const std::string& text, std::size_t open) {
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '\\') ++i;
        else if (text[i] == '"') return i;
    }
    return std::string::npos;
}

// [start, end] of the object value of the TOP-LEVEL member `key`. A structural scan: a string value "video" or a nested
// "video" key earlier in the text is not mistaken for it (mpv_config_safety's first-occurrence search would be).
bool FindTopLevelObject(const std::string& text, const std::string& key, std::size_t* start, std::size_t* end) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; continue; }
        if (c != '"') continue;
        const std::size_t open = i;
        i = StringEnd(text, open);
        if (i == std::string::npos) return false;
        if (depth != 1 || i - open - 1 != key.size() || text.compare(open + 1, key.size(), key) != 0) continue;
        const std::size_t colon = text.find_first_not_of(" \t\r\n", i + 1);
        if (colon == std::string::npos || text[colon] != ':') continue; // a string value, not a key
        const std::size_t value = text.find_first_not_of(" \t\r\n", colon + 1);
        if (value == std::string::npos || text[value] != '{') return false;
        int inner = 0;
        for (std::size_t j = value; j < text.size(); ++j) {
            const char d = text[j];
            if (d == '"') {
                j = StringEnd(text, j);
                if (j == std::string::npos) return false;
            } else if (d == '{' || d == '[') {
                ++inner;
            } else if ((d == '}' || d == ']') && --inner == 0) {
                *start = value;
                *end = j;
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace

std::vector<AdapterInfo> EnumerateAdapters() {
    std::vector<AdapterInfo> adapters;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return adapters;
    std::uint32_t hardware_index = 0;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND || FAILED(result)) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            AdapterInfo info;
            info.luid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                        static_cast<std::uint32_t>(desc.AdapterLuid.LowPart);
            info.vendor = desc.VendorId;
            info.device = desc.DeviceId;
            info.subsys = desc.SubSysId;
            info.revision = desc.Revision;
            info.name = Utf8(desc.Description);
            info.vram = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
            info.dxgi_index = index;
            info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.hardware_index = info.software ? 0 : hardware_index++;
            adapters.push_back(std::move(info));
        }
        adapter->Release();
    }
    factory->Release();
    return adapters;
}

SavedSelection ParseSavedSelection(const Json& config) {
    SavedSelection saved;
    const Json& video = config["video"];
    if (video["gpuAdapterLuid"].is_string()) {
        saved.luid_text = video["gpuAdapterLuid"].str();
    } else {
        saved.luid_text = config["gpuAdapterLuid"].str();
        saved.source = "legacy_top_level";
    }
    const Json& identity = video["gpuAdapterIdentity"];
    if (saved.source == "video" && identity.is_object() && identity["v"].type() == Json::Type::Number && identity["v"].i64() == 1 &&
        NumberMember(identity, "vendorId", &saved.vendor) && NumberMember(identity, "deviceId", &saved.device) &&
        NumberMember(identity, "subSysId", &saved.subsys) && NumberMember(identity, "revision", &saved.revision)) {
        saved.has_identity = true;
        saved.name = identity["name"].str();
    }
    return saved;
}

int MpvD3d11PrefixWinner(const std::vector<AdapterInfo>& adapters, const std::string& name) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(adapters.size()); ++i) {
        if (!CaseStartsWith(adapters[i].name, name)) continue;
        if (best < 0 || adapters[i].dxgi_index < adapters[best].dxgi_index) best = i;
    }
    return best;
}

bool MpvAddressable(const std::vector<AdapterInfo>& adapters, int index) {
    return index >= 0 && index < static_cast<int>(adapters.size()) && MpvD3d11PrefixWinner(adapters, adapters[index].name) == index;
}

std::uint32_t CudaOrdinal(const std::vector<AdapterInfo>& adapters, std::uint32_t hardware_index) {
    std::uint32_t nvidia_before = 0;
    for (const auto& adapter : adapters)
        if (!adapter.software && adapter.hardware_index < hardware_index && adapter.vendor == 0x10DE) ++nvidia_before;
    return nvidia_before;
}

Resolution Reconcile(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, bool d3d11_render) {
    Resolution resolution;
    const int count = static_cast<int>(adapters.size());
    if (saved.luid_text.empty() || saved.luid_text == "auto") {
        // Automatic: the first DXGI hardware adapter (the Windows default GPU), no d3d11-adapter, no identity check.
        resolution.outcome = Outcome::Auto;
        for (int i = 0; i < count && resolution.adapter < 0; ++i)
            if (!adapters[i].software) resolution.adapter = i;
        resolution.reason = resolution.adapter < 0 ? "no_hardware_adapter" : "system_default";
        return resolution;
    }
    char* end = nullptr;
    const unsigned long long wanted = std::strtoull(saved.luid_text.c_str(), &end, 10);
    if (!end || end == saved.luid_text.c_str() || *end != '\0' || wanted == 0 || saved.luid_text[0] == '-' || saved.luid_text[0] == '+') {
        resolution.outcome = Outcome::Invalid;
        resolution.reason = "invalid_value";
        return resolution;
    }
    int by_luid = -1;
    for (int i = 0; i < count && by_luid < 0; ++i)
        if (!adapters[i].software && adapters[i].luid == wanted) by_luid = i;
    bool resolved = false;
    if (by_luid >= 0) {
        if (!saved.has_identity) {
            resolution.outcome = Outcome::ExactUnverified;
            resolution.adapter = by_luid;
            resolution.reason = "luid_match_no_identity";
            resolved = true;
        } else if (KeyMatches(saved, adapters[by_luid])) {
            resolution.outcome = Outcome::Exact;
            resolution.adapter = by_luid;
            resolution.reason = "luid_and_identity_match";
            resolved = true;
        } else {
            resolution.reason = "luid_reused_by_other_adapter";
        }
    } else {
        resolution.reason = "luid_stale";
    }
    if (!resolved) {
        if (!saved.has_identity) {
            resolution.outcome = Outcome::NotFound;
            resolution.reason += "_no_identity";
            return resolution;
        }
        std::vector<int> matches;
        for (int i = 0; i < count; ++i)
            if (!adapters[i].software && KeyMatches(saved, adapters[i])) matches.push_back(i);
        if (matches.size() == 1) {
            resolution.outcome = Outcome::Reconciled;
            resolution.adapter = matches.front();
            resolution.reason += "_identity_unique";
        } else if (matches.size() > 1) {
            resolution.outcome = Outcome::Ambiguous;
            resolution.candidates = matches;
            resolution.reason += "_identity_ambiguous";
            return resolution;
        } else {
            resolution.outcome = Outcome::NotFound;
            resolution.reason += "_identity_absent";
            int suggestion = -1, similar = 0;
            for (int i = 0; i < count; ++i)
                if (!adapters[i].software && adapters[i].vendor == saved.vendor && adapters[i].device == saved.device) { suggestion = i; ++similar; }
            if (similar == 1) resolution.suggestion = suggestion;
            return resolution;
        }
    }
    // Explicit selection rendered through d3d11: mpv must be able to pick exactly this adapter by name.
    if (d3d11_render) {
        const int winner = MpvD3d11PrefixWinner(adapters, adapters[resolution.adapter].name);
        if (winner != resolution.adapter) {
            resolution.outcome = Outcome::NotAddressable;
            resolution.prefix_winner = winner;
            resolution.reason = "mpv_prefix_match_selects_other_adapter";
        }
    }
    return resolution;
}

const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::Auto: return "auto";
    case Outcome::Exact: return "exact";
    case Outcome::ExactUnverified: return "exact_unverified";
    case Outcome::Reconciled: return "reconciled";
    case Outcome::NotFound: return "not_found";
    case Outcome::Ambiguous: return "ambiguous";
    case Outcome::NotAddressable: return "not_addressable";
    case Outcome::Invalid: return "invalid";
    }
    return "unknown";
}

bool IsBlocking(Outcome outcome) {
    return outcome == Outcome::NotFound || outcome == Outcome::Ambiguous || outcome == Outcome::NotAddressable ||
           outcome == Outcome::Invalid;
}

const char* ErrorCodeFor(Outcome outcome) {
    switch (outcome) {
    case Outcome::NotFound: case Outcome::Invalid: return "GPU_ADAPTER_NOT_FOUND";
    case Outcome::Ambiguous: return "GPU_ADAPTER_AMBIGUOUS";
    case Outcome::NotAddressable: return "GPU_ADAPTER_NOT_ADDRESSABLE";
    default: return "";
    }
}

std::string UserMessage(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution) {
    const std::string name = !saved.name.empty() ? saved.name : NameOf(adapters, resolution.adapter);
    const std::string named = name.empty() ? std::string() : "（" + name + "）";
    switch (resolution.outcome) {
    case Outcome::NotFound: case Outcome::Invalid:
        return "找不到先前選擇的 GPU" + named + "。請到「設定 → 影像」重新選擇，或改用自動；選擇後立即生效，不需重新啟動。";
    case Outcome::Ambiguous:
        return "偵測到 " + std::to_string(resolution.candidates.size()) + " 張與先前選擇相同型號的 GPU" + named +
               "，無法確定是哪一張。請到「設定 → 影像」重新選擇，或改用自動。";
    case Outcome::NotAddressable:
        return "mpv 只能依名稱指定 GPU，所選的「" + NameOf(adapters, resolution.adapter) + "」會被同名或名稱開頭相同的「" +
               NameOf(adapters, resolution.prefix_winner) + "」取代。請到「設定 → 影像」改選其他 GPU，或改用自動。";
    default: return {};
    }
}

std::string IdentityJson(const AdapterInfo& adapter) {
    return "{\"v\":1,\"vendorId\":" + std::to_string(adapter.vendor) + ",\"deviceId\":" + std::to_string(adapter.device) +
           ",\"subSysId\":" + std::to_string(adapter.subsys) + ",\"revision\":" + std::to_string(adapter.revision) +
           ",\"name\":" + Json::Quote(adapter.name) + ",\"dedicatedVideoMemory\":" + std::to_string(adapter.vram) + "}";
}

std::string AdapterListJson(const std::vector<AdapterInfo>& adapters) {
    std::string json = "[";
    for (int i = 0; i < static_cast<int>(adapters.size()); ++i) {
        const AdapterInfo& adapter = adapters[i];
        if (adapter.software) continue;
        if (json.size() > 1) json += ",";
        json += "{\"index\":" + std::to_string(adapter.hardware_index) + ",\"luid\":" + Json::Quote(std::to_string(adapter.luid)) +
                ",\"name\":" + Json::Quote(adapter.name) + ",\"vendorId\":" + std::to_string(adapter.vendor) +
                ",\"deviceId\":" + std::to_string(adapter.device) + ",\"subSysId\":" + std::to_string(adapter.subsys) +
                ",\"revision\":" + std::to_string(adapter.revision) + ",\"dedicatedVideoMemory\":" + std::to_string(adapter.vram) +
                ",\"mpvAddressable\":" + (MpvAddressable(adapters, i) ? "true" : "false") + "}";
    }
    return json + "]";
}

std::string StatusJson(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution,
                       bool libmpv_attempted) {
    const bool blocked = IsBlocking(resolution.outcome);
    std::string candidates = "[";
    for (const int index : resolution.candidates) candidates += (candidates.size() > 1 ? "," : "") + Json::Quote(LuidText(adapters, index));
    candidates += "]";
    const int resolved = blocked ? -1 : resolution.adapter;
    return std::string("{\"state\":") + (blocked ? "\"blocked\"" : "\"ok\"") +
           ",\"outcome\":" + Json::Quote(OutcomeName(resolution.outcome)) + ",\"code\":" + Json::Quote(ErrorCodeFor(resolution.outcome)) +
           ",\"reason\":" + Json::Quote(resolution.reason) + ",\"message\":" + Json::Quote(UserMessage(saved, adapters, resolution)) +
           ",\"source\":" + Json::Quote(saved.source) + ",\"savedLuid\":" + Json::Quote(saved.luid_text) +
           ",\"savedName\":" + Json::Quote(saved.name) + ",\"hasIdentity\":" + (saved.has_identity ? "true" : "false") +
           ",\"resolvedLuid\":" + Json::Quote(LuidText(adapters, resolved)) + ",\"resolvedName\":" + Json::Quote(NameOf(adapters, resolved)) +
           ",\"resolvedVendorId\":" + std::to_string(resolved >= 0 ? adapters[resolved].vendor : 0) +
           ",\"candidates\":" + candidates + ",\"suggestionLuid\":" + Json::Quote(LuidText(adapters, resolution.suggestion)) +
           ",\"selectedLuid\":" + Json::Quote(LuidText(adapters, resolution.adapter)) +
           ",\"prefixWinnerName\":" + Json::Quote(NameOf(adapters, resolution.prefix_winner)) +
           ",\"libmpvAttempted\":" + (libmpv_attempted ? "true" : "false") + "}";
}

std::string PreflightLogLine(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution) {
    int hardware = 0;
    for (const auto& adapter : adapters) hardware += adapter.software ? 0 : 1;
    const bool has = resolution.adapter >= 0 && resolution.adapter < static_cast<int>(adapters.size());
    const AdapterInfo* const chosen = has ? &adapters[resolution.adapter] : nullptr;
    std::string line = std::string("gpu adapter preflight outcome=") + OutcomeName(resolution.outcome) + " source=" + saved.source +
        " saved_luid=" + (saved.luid_text.empty() ? std::string("-") : saved.luid_text) + " identity=" +
        (saved.has_identity ? "v1 vendor=" + Hex(saved.vendor, 4) + " device=" + Hex(saved.device, 4) + " subsys=" + Hex(saved.subsys, 8) +
                              " rev=" + std::to_string(saved.revision) : std::string("none")) +
        " resolved_luid=" + (chosen ? std::to_string(chosen->luid) : std::string("-")) +
        " hw_index=" + (chosen ? std::to_string(chosen->hardware_index) : std::string("-")) +
        " dxgi_index=" + (chosen ? std::to_string(chosen->dxgi_index) : std::string("-")) +
        " name=\"" + (chosen ? chosen->name : std::string()) + "\" candidates=" + std::to_string(resolution.candidates.size()) +
        " adapters=" + std::to_string(hardware) + " reason=" + resolution.reason;
    if (resolution.outcome == Outcome::NotAddressable)
        line += "; not addressable by d3d11-adapter: selected dxgi_index=" + std::to_string(chosen ? chosen->dxgi_index : 0) +
                " but mpv prefix match picks dxgi_index=" +
                (resolution.prefix_winner >= 0 ? std::to_string(adapters[resolution.prefix_winner].dxgi_index) : std::string("-")) +
                " \"" + NameOf(adapters, resolution.prefix_winner) + "\"";
    if (resolution.suggestion >= 0) line += " suggestion_luid=" + std::to_string(adapters[resolution.suggestion].luid);
    return line;
}

bool SetVideoGpuAdapterText(const std::string& view_text, const std::string& luid, const std::string& identity_json,
                            std::string* out) {
    if (!out) return false;
    bool ok = false;
    const Json view = Json::Parse(view_text, &ok);
    if (!ok || !view.is_object() || !view["video"].is_object()) return false;
    bool identity_ok = true;
    const Json identity = identity_json.empty() ? Json() : Json::Parse(identity_json, &identity_ok);
    if (!identity_ok || (!identity_json.empty() && !identity.is_object())) return false;
    std::string video = "{";
    bool wrote_luid = false, wrote_identity = false;
    const auto add = [&video](const std::string& key, const std::string& value) {
        video += (video.size() > 1 ? "," : "") + Json::Quote(key) + ":" + value;
    };
    for (const auto& [key, value] : view["video"].members()) {
        if (key == "gpuAdapterLuid") { add(key, Json::Quote(luid)); wrote_luid = true; }
        else if (key == "gpuAdapterIdentity") { if (!identity_json.empty()) { add(key, identity_json); wrote_identity = true; } }
        else add(key, mpv_config_safety::SerializeJson(value));
    }
    if (!wrote_luid) add("gpuAdapterLuid", Json::Quote(luid));
    if (!wrote_identity && !identity_json.empty()) add("gpuAdapterIdentity", identity_json);
    video += "}";
    std::size_t start = 0, end = 0;
    if (!FindTopLevelObject(view_text, "video", &start, &end)) return false;
    std::string updated = view_text;
    updated.replace(start, end - start + 1, video);
    // Verify: only video.gpuAdapterLuid / video.gpuAdapterIdentity changed.
    bool updated_ok = false;
    const Json after = Json::Parse(updated, &updated_ok);
    if (!updated_ok || !after.is_object() || after.members().size() != view.members().size()) return false;
    for (std::size_t i = 0; i < view.members().size(); ++i) {
        const auto& [key, before_value] = view.members()[i];
        if (after.members()[i].first != key) return false;
        if (key == "video") continue;
        if (mpv_config_safety::SerializeJson(before_value) != mpv_config_safety::SerializeJson(after.members()[i].second)) return false;
    }
    const Json& after_video = after["video"];
    if (!after_video.is_object() || after_video["gpuAdapterLuid"].str("\x01") != luid) return false;
    if (identity_json.empty() ? !after_video["gpuAdapterIdentity"].is_null()
                              : mpv_config_safety::SerializeJson(after_video["gpuAdapterIdentity"]) != mpv_config_safety::SerializeJson(identity))
        return false;
    for (const auto& [key, value] : view["video"].members()) {
        if (key == "gpuAdapterLuid" || key == "gpuAdapterIdentity") continue;
        if (mpv_config_safety::SerializeJson(value) != mpv_config_safety::SerializeJson(after_video[key])) return false;
    }
    *out = updated;
    return true;
}

} // namespace vp::gpu
