// Shared MPV view/stored config transforms used by the player and acceptance harness.
#pragma once

#include "json.h"

#include <string>

namespace vp::mpv_config_safety {

std::string SerializeJson(const Json& value);
bool HasMember(const Json& object, const std::string& key);
bool PreservePlaybackCoreConfig(const Json& before, const Json& after);

// Exposed so the acceptance harness exercises the same text-preserving transform as the host.
std::string SetObjectMemberText(std::string text, const std::string& key, const std::string& value);
std::string MakeMpvViewText(const std::string& raw_config_text);
bool BuildStoredMpvConfigText(const std::string& raw_config_text,
                              const std::string& view_config_text,
                              std::string* stored_config_text);

} // namespace vp::mpv_config_safety
