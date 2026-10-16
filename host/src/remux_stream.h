// Browser streams for LAN clients without the native player (Android phones / TV boxes).
//
// The server does the container and audio work so a plain <video> element can play the file:
//   * video is copied (H.264 / HEVC / AV1 / VP9) into fragmented MP4 - no re-encode;
//   * audio that browsers cannot decode (FLAC, AC-3, DTS, TrueHD, PCM, ...) becomes stereo AAC;
//   * embedded text subtitles are exported as ASS for the page's libass (jassub) renderer;
//   * embedded fonts are extracted once into data\cache\fonts\<key>\.
// `sr=1` and/or `frc=1` decode and process video on the server, then emit video-only H.264
// fragmented MP4. Processed streaming is limited to one active stream and supported SDR output.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vp::lan { struct LanHooks; }

namespace vp::remux {

using WriteFn = std::function<bool(const std::uint8_t* data, std::size_t size)>;

struct StreamProcessingSettings {
    std::int32_t mode = 1;
    std::int32_t animejanai_model = 0;
    std::int32_t inference_height = 720;
    std::int32_t interpolation_backend = 1;
    bool rife_restore = false;
    std::string rife_restore_model;
    bool rife_output_warp = false;
    bool frc_dedup = false;
};
using StreamProcessingSettingsFn = std::function<StreamProcessingSettings()>;

// {"durationMs", "video":{codec,profile,width,height,pixFmt,fps,browser}, "audio":[...], "subtitles":[...], "fonts":[...]}
// fonts are paths relative to cache_dir (served as /cache/<path>). Empty string on failure.
std::string ProbeJson(const std::filesystem::path& file, const std::filesystem::path& cache_dir);

// {"startMs": media time (ms from the file start) at output time 0 of /stream/remux?start=ms}. It is
// the decode time of the key frame at/before ms and may be slightly negative. Empty string on failure.
std::string SeekPointJson(const std::filesystem::path& file, std::int64_t ms);

// Writes fragmented MP4 from the key frame at/before start_ms; output time 0 = that key frame.
// audio_stream: absolute stream index, or -1 for the default track. Returns false on failure
// before any byte was written (error describes it); a client disconnect simply ends the stream.
// `begin` is called once with the MIME type just before the first byte.
bool Remux(const std::filesystem::path& file, std::int64_t start_ms, int audio_stream, const std::function<bool(const std::string& mime)>& begin,
           const WriteFn& write, std::string* error);

// Experimental, video-only server-processed stream. PlaybackCore applies the requested
// upscaling/interpolation stages; the host converts output surfaces to H.264 fragmented MP4.
bool ProcessedVideoStream(const std::filesystem::path& file, std::int64_t start_ms,
                          int audio_stream, bool enable_upscale, bool enable_interpolation,
                          const StreamProcessingSettings& settings,
                          const std::function<bool(const std::string& mime)>& begin,
                          const WriteFn& write, std::string* error, std::string* diagnostics);

// The whole embedded text subtitle track (absolute stream index) as an ASS script, cached in
// cache_dir\subs. Empty string when the stream is not a text subtitle.
std::string SubtitleAss(const std::filesystem::path& file, int stream, const std::filesystem::path& cache_dir);

// Fills hooks.remux / probe / subtitle / seekpoint for the LAN server.
// /stream/remux?p=&start=<ms>&audio=<stream index>[&sr=1][&frc=1]
void InstallLanHooks(vp::lan::LanHooks& hooks, const std::filesystem::path& cache_dir,
                     StreamProcessingSettingsFn processing_settings = {});

} // namespace vp::remux
