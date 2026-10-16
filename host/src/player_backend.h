#pragma once

#include "json.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace vp {

// The host consumes player events on its UI thread. Backend callbacks must only
// wake that thread; they must not call WebView2 or mutate window state directly.
class PlayerBackend {
public:
    // Returns 0 when the wake was delivered (posted), else the Win32 error (core-reliability 1B: a failed post is
    // retried by the backend instead of silently stopping event processing).
    using WakeFn = std::function<unsigned long()>;
    using EventFn = std::function<void(const std::string& name, const std::string& json)>;

    virtual ~PlayerBackend() = default;
    virtual bool Initialize(HWND host, HWND video_child, const std::filesystem::path& exe_dir,
                            const std::filesystem::path& data_dir, const std::string& config_json,
                            WakeFn wake, EventFn event, std::string* error) = 0;
    virtual void PollEvents() = 0;
    virtual int Open(const std::string& path, std::int64_t start_ms, bool paused, bool no_audio) = 0;
    virtual int Play() = 0;
    virtual int Pause() = 0;
    virtual int Toggle() = 0;
    virtual int Seek(std::int64_t ms) = 0;
    virtual int SeekRelative(std::int64_t ms) = 0;
    virtual int SelectSubtitle(std::int64_t id) = 0;
    virtual int LoadSubtitle(const std::string& path) = 0;
    virtual int SelectAudio(std::int64_t id) = 0;
    virtual int Stop() = 0;
    virtual void Resize(int width, int height) = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;
    virtual std::string WasapiDevicesJson() = 0;
    virtual std::string PropertyString(const char* name) = 0;
    virtual double PropertyDouble(const char* name, double fallback = 0.0) = 0;
    virtual bool SetPropertyString(const char* name, const std::string& value) = 0;
    virtual bool Command(const std::vector<std::string>& args) = 0;
    virtual bool ApplyEnhancement(const Json& video_config) = 0;
    virtual bool ConfigureAudio(const Json& audio_config) = 0;
};

} // namespace vp
