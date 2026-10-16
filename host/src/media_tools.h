#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vp::media_tools {

enum class ThumbnailEvent : int { Info = 1, Image = 2, Error = 3 };
using ThumbnailCallback = std::function<int(ThumbnailEvent kind, std::int64_t value_ms, const std::string& text)>;

int GenerateThumbnails(const std::string& media_utf8, const std::string& out_dir_utf8,
                       std::int64_t interval_ms, int width, const ThumbnailCallback& callback);
int GenerateThumbnailsAt(const std::string& media_utf8, const std::string& out_dir_utf8,
                         std::vector<std::int64_t> times_ms, int width, bool allow_hardware,
                         const ThumbnailCallback& callback);
int ExtractArtwork(const std::string& media_utf8, const std::string& out_dir_utf8,
                  const ThumbnailCallback& callback);

} // namespace vp::media_tools
