#include "mpv_config_safety.h"

#include "lan_server.h"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace vp::mpv_config_safety {
namespace {

bool FindObjectMember(const std::string& text, const std::string& key,
                     std::size_t* start, std::size_t* end) {
    const std::string quoted = Json::Quote(key);
    const auto at = text.find(quoted);
    if (at == std::string::npos) return false;
    const std::size_t open = text.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (open == std::string::npos || text[open] != '{') return false;
    std::size_t depth = 0;
    bool in_string = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { *start = open; *end = i; return true; }
    }
    return false;
}

std::string SetObjectMember(std::string text, const std::string& key, const std::string& value) {
    std::size_t start = 0, end = 0;
    if (FindObjectMember(text, key, &start, &end)) return text.replace(start, end - start + 1, value);
    const auto brace = text.find('{');
    if (brace == std::string::npos) return text;
    const auto next = text.find_first_not_of(" \t\r\n", brace + 1);
    const bool empty_object = next != std::string::npos && text[next] == '}';
    return text.insert(brace + 1, "\n  " + Json::Quote(key) + ": " + value + (empty_object ? "" : ","));
}

std::string MpvLanSection(const Json& config) {
    if (config["mpvLan"].is_object()) return SerializeJson(config["mpvLan"]);
    const Json& lan = config["lan"];
    const std::string name = lan["name"].str();
    return "{\"mode\":" + Json::Quote(lan["mode"].str() == "server" ? "server" : "standalone") +
           ",\"port\":" + std::to_string(lan::kDefaultHttpPort + 1) +
           ",\"name\":" + Json::Quote(name.empty() ? std::string("SceneArc (MPV)") : name + " (MPV)") + "}";
}

} // namespace

bool HasMember(const Json& object, const std::string& key) {
    return std::any_of(object.members().begin(), object.members().end(),
                       [&](const auto& member) { return member.first == key; });
}

std::string SerializeJson(const Json& value) {
    switch (value.type()) {
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return value.boolean() ? "true" : "false";
    case Json::Type::Number: {
        std::ostringstream out;
        out << std::setprecision(17) << value.num();
        return out.str();
    }
    case Json::Type::String: return Json::Quote(value.str());
    case Json::Type::Array: {
        std::string out = "[";
        for (const auto& item : value.items()) out += (out.size() > 1 ? "," : "") + SerializeJson(item);
        return out + "]";
    }
    case Json::Type::Object: {
        std::string out = "{";
        for (const auto& [key, item] : value.members())
            out += (out.size() > 1 ? "," : "") + Json::Quote(key) + ":" + SerializeJson(item);
        return out + "}";
    }
    }
    return "null";
}

bool PreservePlaybackCoreConfig(const Json& before, const Json& after) {
    // General app settings may change; keys consumed by the PlaybackCore build
    // must remain semantically identical in the parallel MPV configuration.
    // "lan" is deliberately not shared: the MPV edition keeps its LAN settings in "mpvLan".
    // "playback" (seekMode) is read only by the MPV edition (MpvBackend::SeekUser).
    static const std::set<std::string> shared = {"video", "audio", "playback", "mpvLan", "libraries", "startFullscreen"};
    // UI-only preferences stored in config.json: both editions run the same UI, which is the only
    // reader of these keys, so a change from the MPV edition means the same thing to PlaybackCore.
    // "theme" is the colour theme picked on 設定 → 介面 (light / crimson / graphite / sakura).
    static const std::set<std::string> ui_shared = {"theme"};
    for (const auto& [key, old_value] : before.members()) {
        if (shared.contains(key) || ui_shared.contains(key)) continue;
        if (!HasMember(after, key) || SerializeJson(old_value) != SerializeJson(after[key])) return false;
    }
    for (const auto& [key, new_value] : after.members()) {
        if (shared.contains(key) || ui_shared.contains(key) || HasMember(before, key)) continue;
        // A config without "lan" means standalone to the PlaybackCore edition; adding exactly that is harmless.
        if (key == "lan" && SerializeJson(new_value) == "{\"mode\":\"standalone\"}") continue;
        return false;
    }
    return HasMember(after, "video") && HasMember(after, "audio");
}

std::string SetObjectMemberText(std::string text, const std::string& key, const std::string& value) {
    return SetObjectMember(std::move(text), key, value);
}

std::string MakeMpvViewText(const std::string& raw_config_text) {
    bool ok = false;
    const Json config = Json::Parse(raw_config_text, &ok);
    if (!ok || !config.is_object()) return raw_config_text;
    return SetObjectMember(raw_config_text, "lan", MpvLanSection(config));
}

bool BuildStoredMpvConfigText(const std::string& raw_config_text,
                              const std::string& view_config_text,
                              std::string* stored_config_text) {
    if (!stored_config_text) return false;
    bool before_ok = false, view_ok = false;
    const Json before = Json::Parse(raw_config_text, &before_ok);
    const Json view = Json::Parse(view_config_text, &view_ok);
    if (!before_ok || !before.is_object() || !view_ok || !view.is_object()) return false;

    // The UI posts the entire config after a settings change. Only persist MPV LAN
    // when the MPV view's LAN section actually changed; an enhancement-only save
    // must not materialize or modify mpvLan.
    bool current_mpv_lan_ok = false;
    const Json current_mpv_lan = Json::Parse(MpvLanSection(before), &current_mpv_lan_ok);
    if (!current_mpv_lan_ok || !current_mpv_lan.is_object()) return false;
    const bool lan_page_changed = view["lan"].is_object() &&
        SerializeJson(view["lan"]) != SerializeJson(current_mpv_lan);

    std::string stored = view_config_text;
    if (lan_page_changed) {
        stored = SetObjectMember(std::move(stored), "mpvLan", SerializeJson(view["lan"]));
    } else if (HasMember(before, "mpvLan")) {
        // Ignore accidental edits to the hidden storage key; the UI edits its "lan" view.
        stored = SetObjectMember(std::move(stored), "mpvLan", SerializeJson(before["mpvLan"]));
    } else if (HasMember(view, "mpvLan")) {
        return false;
    }

    const std::string production_lan = HasMember(before, "lan")
        ? SerializeJson(before["lan"]) : std::string("{\"mode\":\"standalone\"}");
    stored = SetObjectMember(std::move(stored), "lan", production_lan);

    bool after_ok = false;
    const Json after = Json::Parse(stored, &after_ok);
    if (!after_ok || !PreservePlaybackCoreConfig(before, after)) return false;
    *stored_config_text = std::move(stored);
    return true;
}

} // namespace vp::mpv_config_safety
