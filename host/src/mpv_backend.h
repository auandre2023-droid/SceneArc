#pragma once

#include "player_backend.h"
#include "enhancement_compiler.h"
#include "gpu_adapter_identity.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

struct mpv_handle;
struct mpv_event;
struct mpv_node;

namespace vp {

class OwnedNode;  // mpv_backend.cpp: a deep copy of an mpv_node (frameProcessing snapshot)
class NodeHolder; // mpv_backend.cpp: one node read (libmpv-owned or borrowed from the snapshot)

// TensorRT inference runtime (mpv-lazy vsNV layout) next to the exe: vs-runtime/vs-plugins/vstrt.dll plus the
// vsmlrt-cuda/ folder with trtexec.exe, where vsmlrt.py looks for it (trtexec_path = plugins_path/vsmlrt-cuda/trtexec),
// and at least one nvinfer*.dll and one cudart64_*.dll in vsmlrt-cuda/ (what vstrt and trtexec load).
// On failure *missing names the first absent file or folder.
bool TensorRtRuntimeInstalled(const std::filesystem::path& exe_dir, std::string* missing = nullptr);

// TensorRT model exports k7sfunc's DRBA_NV / RIFE_NV load from vs-runtime/vs-plugins/models (paths relative to models/,
// with forward slashes). DRBA: turbo 1 = drba/<model>_ap.onnx, turbo 2 = drba/<model>_scale_ap.onnx (no _fp16 under TRT).
// RIFE: rife_v2/ (fast) and rife/ (quality) are the same files DirectML uses.
struct TensorRtModelExports {
    std::vector<std::string> present;
    std::vector<std::string> missing;
};
TensorRtModelExports TensorRtModelExportsPresent(const std::filesystem::path& exe_dir);

// Bit-perfect / volume / frame-sync coupling (one decision for init, ConfigureAudio, ApplyEnhancement, the per-file
// preload pass and container-fps / display-fps changes). Pure: inputs in, properties to set out.
struct AudioSyncPolicy {
    bool bitperfect{};          // audio.exclusive: WASAPI exclusive + fixed 100 % + audio master clock
    int volume{100};            // mpv `volume` to output (forced 100 when bitperfect)
    bool mute{};                // user mute (stays user-controllable in both modes)
    std::string video_sync;     // audio | display-resample
    bool interpolation{};       // mpv `interpolation` (mpv 平滑 only, and only under a display-sync mode)
    std::string requested_clock; // audio.masterClock as resolved: audio (音訊優先) | auto (mpv-lazy vsync_auto)
    std::string effective_clock; // audio | display (display-resample: the display drives, the audio is resampled)
    std::string reason;         // bitperfect | bitperfect_no_smooth | clock_audio | clock_audio_no_smooth |
                                // speed_not_one | fps_unknown | fps_gt_32 | ratio_gt_3.2 | vsync_auto
};
// master_clock: audio.masterClock ("audio" = always video-sync=audio; anything else = "auto", mpv-lazy's shipped
// [vsync_auto] rule with no host override for filter interpolators). Bit-perfect always wins (audio clock).
AudioSyncPolicy ResolveAudioSyncPolicy(bool bitperfect, const std::string& master_clock, int saved_volume, bool mute,
                                       const std::string& interpolation_mode, double container_fps, double display_fps,
                                       double speed);
// audio.masterClock normalised: "audio" | "auto" (missing or unknown values: "auto").
std::string NormalizeMasterClock(const std::string& value);

class MpvBackend final : public PlayerBackend {
public:
    MpvBackend();
    ~MpvBackend() override;

    bool Initialize(HWND host, HWND video_child, const std::filesystem::path& exe_dir,
                    const std::filesystem::path& data_dir, const std::string& config_json,
                    WakeFn wake, EventFn event, std::string* error) override;
    void PollEvents() override;
    int Open(const std::string& path, std::int64_t start_ms, bool paused, bool no_audio) override;
    int Play() override;
    int Pause() override;
    int Toggle() override;
    int Seek(std::int64_t ms) override;          // absolute+exact (chapter starts, restart)
    int SeekRelative(std::int64_t ms) override;  // relative+exact
    // User seeks (seek bar, arrows, J/L, ±10 s, digits/Home/End, LAN relays, the no-chapter ±60 s fallback): the
    // playback.seekMode preference resolved here (exact | keyframes | nopreview; nopreview is a UI presentation policy
    // and seeks exactly). exact=true forces absolute/relative+exact whatever the preference.
    int SeekUser(std::int64_t ms, bool relative, bool exact);
    // exact | keyframes | nopreview. Legacy "auto" and anything else: exact.
    void SetSeekPreference(const std::string& mode);
    const std::string& SeekPreference() const { return seek_pref_; }
    // Volume / mute from the player bar (setVolume / setMute). Volume is the saved user level (0..130); while
    // bit-perfect is on it is stored but the output stays at 100. Never touches vf or the VapourSynth chain.
    void SetVolume(int value);
    void SetMute(bool value);
    // HDR output (video.hdrOutput): the display capability cache under the window is re-read on the next
    // frameProcessing tick after this (main.cpp: WM_DISPLAYCHANGE, WM_MOVE), else every few seconds while a file is open.
    void InvalidateDisplayCaps() { display_caps_dirty_ = true; }
    int SelectSubtitle(std::int64_t id) override;
    int LoadSubtitle(const std::string& path) override;
    int SelectAudio(std::int64_t id) override;
    int Stop() override;
    void Resize(int width, int height) override;
    void Shutdown() override; // waits for libmpv at most 10 s (see BeginShutdown)
    // Stops all libmpv use and starts mpv_terminate_destroy on a worker thread; returns at once.
    void BeginShutdown();
    // Waits up to ms for the worker started by BeginShutdown; true when libmpv is gone (or was never started).
    bool AwaitShutdown(DWORD ms);
    bool Ready() const override { return ctx_ != nullptr && initialized_; }
    bool InitializationAttempted() const { return initialize_attempted_; }
    // Core-reliability 1A: Initialize validated the saved GPU before the process-scoped libmpv attempt and stopped
    // (NOT_FOUND / AMBIGUOUS / NOT_ADDRESSABLE). InitializationAttempted() stays false, so the next EnsurePlayer after a
    // re-selection initialises in this process.
    bool AdapterPreflightBlocked() const { return adapter_preflight_blocked_; }
    // gpuAdapterStatus payload of the last preflight ("" before the first one).
    std::string AdapterStatusJson() const;
    // Reconciled (stale LUID, unique identity match) or legacy LUID without identity: once, the adapter main.cpp persists.
    struct AdapterPersist { gpu::AdapterInfo adapter; std::string outcome; std::string old_luid; std::string source; };
    bool TakeAdapterPersist(AdapterPersist* persist);
    // Core-reliability 2A: libmpv's terminate_destroy was still running when Shutdown gave up (kShutdownWaitMs).
    bool ShutdownDetached() const { return shutdown_detached_; }
    const std::string& ErrorCode() const { return error_code_; }
    std::string WasapiDevicesJson() override;
    std::string PropertyString(const char* name) override;
    double PropertyDouble(const char* name, double fallback = 0.0) override;
    bool SetPropertyString(const char* name, const std::string& value) override;
    bool Command(const std::vector<std::string>& args) override;
    bool ApplyEnhancement(const Json& video_config) override;
    bool ConfigureAudio(const Json& audio_config) override;
    // Hides the video child. The UI sends releaseSurface after its exit transition; Stop()
    // itself leaves the last image (black after mpv unloads) on screen under the UI.
    void ReleaseSurface();
    // True from Open() until the file ends or Stop(): the host window erases black meanwhile.
    bool MediaActive() const { return file_loaded_ || pending_open_ || first_frame_pending_; }

    std::string AudioStatusJson() const;
    // Host-side actions (main.cpp) that belong in mpv-host.log next to the playback lines.
    void HostLog(const std::string& line) { AppendLog(line); }
    // True while a VapourSynth chain (re)initialises on mpv's playback thread (script evaluation, ONNX/DirectML
    // session, TensorRT engine build or deserialise). See BeginVsBusy.
    bool VsBusy() const { return vs_busy_; }
    // Called by the vs-busy timer on the host window (1 s): timeout log and the post-apply probe.
    void OnVsBusyTick();
    // Called by the reconfig-watch timer (100 ms) while a mid-stream format change reloads the chain (audit 7.3b).
    void OnVsReconfigTick();
    // Diagnostics export (main.cpp exportDiagnostics; UI thread only): the last emitted event payloads and the
    // resolved state they came from. Empty strings when nothing was emitted yet.
    const std::string& LastFrameProcessingJson() const { return last_frame_processing_json_; }
    const std::string& LastOpenedJson() const { return last_opened_json_; }
    const std::string& AiBackend() const { return ai_backend_; }
    const std::string& AiBackendReason() const { return ai_backend_reason_; }
    bool TrtInstalledAtResolve() const { return trt_installed_; }
    const std::string& VsBusyTrigger() const { return vs_busy_trigger_; }
    double VsBusyMs() const {
        return vs_busy_ ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vs_busy_since_).count() : 0.0;
    }
    const std::string& MpvVersion() const { return mpv_version_; }     // mpv-version property, read at Initialize
    const std::string& FfmpegVersion() const { return ffmpeg_version_; }
    const std::string& GpuVendor() const { return gpu_vendor_; }
    const std::string& CurrentPath() const { return current_path_; }
    const std::string& ActiveVsStages() const { return vs_script_; }   // e.g. "upscale + interpolation (one script)"
    const std::string& VsUpscaleScript() const { return vs_up_script_; } // generated .vpy names in the current chain
    const std::string& VsInterpolationScript() const { return vs_fi_script_; }
    const std::string& VsChainScript() const { return vs_chain_script_; }
#if defined(VP_TEST_HARNESS)
    void DebugInjectLogLine(const std::string& line);
#endif

private:
    // --- UI-thread stall guard (audit C1 / row 4) ---------------------------------------------------------------
    // Every libmpv call that goes through mpv's core lock (mpv_get_property, mpv_set_property, mpv_command,
    // mpv_hook_continue, mpv_terminate_destroy) waits while the playback thread is inside a VapourSynth filter's
    // script load. vs_busy_ is set when a VS chain is (re)applied, or a file is loaded / seeked with a VS stage,
    // and cleared on MPV_EVENT_PLAYBACK_RESTART (or END_FILE, or the post-apply probe). While it is set:
    //   - reads come from the observed-property cache (CachedProperty) or are skipped (node reads, frameProcessing);
    //   - writes go through mpv_set_property_async / mpv_command_async (errors logged from the reply events);
    //   - opened / subtitle / enhancement work that needs fresh reads is deferred to EndVsBusy.
    // Inside the on_preloaded hook mpv's core is parked waiting for us, so synchronous calls stay allowed there.
    bool SyncAllowed() const { return !vs_busy_ || preloading_; }
    // Busy-state transitions (audit 7.3a). One entry and one exit path; 7.3b (mid-stream reconfig) reuses both:
    //   BeginVsBusy(trigger)          enter (or join) with a reason: load | apply | seek | reconfig (7.3b).
    //   RequestVsBusyEnd(by, barrier) an end condition was met (playback-restart, the apply probe, ...). The end is
    //                                 refused while an executed seek still awaits its PLAYBACK_RESTART; it goes through
    //                                 the probe barrier when `barrier` is set or seek commands were issued that mpv has
    //                                 not reported as MPV_EVENT_SEEK yet (coalesced / still queued); inside PollEvents'
    //                                 drain it is only recorded and FinishVsBusy runs after the drain, so an
    //                                 MPV_EVENT_SEEK queued behind the end condition re-joins the busy period first.
    //   FinishVsBusy(by)              leave: clears the state, then runs the deferred synchronous work.
    //   EndVsBusy(by)                 immediate leave without conditions (END_FILE, shutdown).
    // Probe barrier: two chained mpv_get_property_async requests. mpv serves async requests on its playback thread
    // between loop iterations, in order with our earlier commands; the second request is served only after the
    // iteration that processed the first has executed any queued seek (MPV_EVENT_SEEK is then ahead of its reply in
    // the event queue) and, if a seek started a script reload, after that reload. No UI-thread wait at any point.
    void BeginVsBusy(const char* trigger);
    // Audit 7.3b: a mid-stream video-params change (size, pixel format, static colour) while a working VapourSynth stage
    // is in vf makes vf_vapoursynth drain (core lock free, ~0.6 s) and then re-evaluate its script (core lock held: DML
    // ~0.3-1 s, TensorRT deserialise or engine build). Called from the PROPERTY_CHANGE event's own node (no libmpv read)
    // before the handlers run, so they already see the busy state. Ends (through RequestVsBusyEnd with the barrier) on:
    //   - the first MPV_EVENT_VIDEO_RECONFIG after the begin: mpv emits it for the filter OUTPUT change, i.e. after the
    //     reload (the one for the input change precedes the property notification and is never seen as busy);
    //   - a reconfig-watch probe (kind 4, every 100 ms) that waited >= kReconfigReloadObservedMs: the reload was observed
    //     and has finished (needed when the output params do not change, e.g. UAI capped to the same H_Max);
    //   - otherwise 3 s after the begin with the last watch probe answered at once (no reload seen).
    // Never on a probe answered during the drain: the drain leaves the core free, so a fast reply proves nothing there.
    void OnVideoParamsEvent(const void* property_event);
    // HDR source class from video-params (sync read; SyncAllowed only): re-applies only the HDR options on a change.
    void RecheckHdrSource(const char* trigger);
    void RequestVsBusyEnd(const char* by, bool barrier = false);
    void FinishVsBusy(const char* by);
    void EndVsBusy(const char* by);
    void SendVsBusyProbe(int kind);
    void EmitVsInit(const char* state, const std::string& note);
    bool VsStageInVf() const;
    // Synchronous mpv_get_property, or MPV_ERROR_PROPERTY_UNAVAILABLE without calling libmpv while !SyncAllowed().
    int GetPropertySync(const char* name, int format, void* data);
    // Fire-and-forget writes: the reply (MPV_EVENT_SET_PROPERTY_REPLY / COMMAND_REPLY) is logged on error.
    bool SetPropertyAsync(const char* name, const std::string& value);
    int SetPropertyNode(const char* name, void* node); // mpv_node*: sync when allowed (result needed), else async (0)
    std::uint64_t NextReplyId(const std::string& what);
    void OnAsyncReply(std::uint64_t id, int error, const char* kind);
    void CacheObservedProperty(const void* property_event);
    bool LoadApi(const std::filesystem::path& dll, std::string* error);
    bool SetOption(const char* name, const std::string& value, bool required, std::string* error);
    void ObserveProperties();
    void OnEvent(::mpv_event* event);
    void OnProperty(const char* name);
    void Emit(const char* name, const std::string& json);
    void EmitState(const char* forced = nullptr);
    void EmitOpened();
    void RefreshOpenedHwdec();
    void EmitPosition(bool force = false);
    void EmitTracks();
    void EmitSubtitleTrack();
    void EmitError(const std::string& code, const std::string& message, bool fatal);
    // frameProcessing (report 23). The tick used to read ~38 properties synchronously on this (UI) thread; two of them
    // (vo-passes, display-names) wait for mpv's VO thread, about one source frame each (~83 ms per tick at 24 fps), and
    // with an AI chain the others wait for the core lock. Now EmitFrameProcessing only requests a snapshot: the same
    // properties, with the same formats, through mpv_get_property_async (served on mpv's playback thread). When the
    // last reply has arrived, BuildFrameProcessing builds and emits the same JSON; while it runs, PropertyString /
    // PropertyDouble / ReadNode answer from the snapshot (display-names and speed from the observed cache, see
    // FpObservedScalar) and never call libmpv. A read the tick did not request is a miss: logged once, then read
    // synchronously as before. A new request replaces one still in flight; a busy period or a file change since the
    // request (fp_epoch_) discards it (FinishVsBusy / the next tick request a fresh one).
    void EmitFrameProcessing();
    void RequestFrameProcessingSnapshot();
    void SendFrameProcessingRequest(char kind, const char* name, int index);
    void OnFrameProcessingReply(std::uint64_t id, int error, const void* property_event);
    void OnFrameProcessingSnapshotComplete();
    void BuildFrameProcessing();
    struct FpSnapshotValue {
        bool ok{};                              // the read succeeded (a failed read answers like a failed sync read)
        std::string text;                       // 'S' reads (MPV_FORMAT_STRING, as mpv_get_property_string)
        double number{};                        // 'D' reads (MPV_FORMAT_DOUBLE)
        std::shared_ptr<const OwnedNode> node;  // 'N' reads (MPV_FORMAT_NODE, deep copy)
        std::chrono::steady_clock::time_point at{}; // reply handled (time-pos: the rate window's sample time)
    };
    const FpSnapshotValue* FpSnapshotFind(char kind, const char* name) const; // nullptr: not in the snapshot
    void FpSnapshotMiss(char kind, const char* name);
    // Node read: from the snapshot inside BuildFrameProcessing, else GetPropertySync into holder (freed by it).
    const ::mpv_node* ReadNode(const char* name, NodeHolder& holder);
    // Throughput window for frameProcessing and sync-trace (see EmitFrameProcessing): emptied on every playback-clock
    // discontinuity (seek, pause, speed, vf change, VapourSynth busy, file change); why = what the trace prints.
    void ResetRateWindow(const char* why);
    bool ObservedFlag(const char* name) const;  // observed-cache flag (no libmpv call); false when unknown
    std::string VsBusyBackendLog() const;        // trt | dml | cpu | dml+cpu ... for the vs-init log line
    // prefix/level are mpv's log-message fields (empty for injected lines); text is one line without the newline.
    void HandleLogLine(const std::string& prefix, const std::string& level, const std::string& text);
    void HandleVapourSynthFailureLine(const std::string& prefix, const std::string& text);
    void MarkVsStageFailed(const std::string& stage, const std::string& detail, const std::string& source);
    void ApplyVsFailureOverrides();
    // Audit 7.2c: every generated VapourSynth script of the current chain failed in this file (false with none).
    bool AllVsScriptsFailed() const;
    // Audit 7.2c: removes a chain whose VapourSynth stages all failed (ApplyEnhancement's suppression rule drops them,
    // restores the configured hwdec first and then sets vf). Runs from the event loop only: after PollEvents' drain and
    // at the end of FinishVsBusy, never from the log handler, never while busy.
    void RunPendingDeadVsCleanup(const char* where);
    // Core-reliability 3A (deinterlace=auto): mpv's own deinterlacer (d3d11vpp on native d3d11va frames, bwdif
    // send_field on copy-back / software frames) runs before vf and outputs the FIELD rate, so a VapourSynth stage
    // receives 2x container-fps as `container_fps` while the container-fps property is unchanged. The 7.2a gate uses
    // that input rate once deinterlace-active was seen in this file (deint_seen_, sticky per file), or once k7sfunc's
    // FPS_CTRL refused the rate at the first frame (vs_fps_limit_seen_: interlacing is unknowable before the first
    // decoded frame, which is also when the script is evaluated). Either sets vs_rate_reapply_pending_; the chain is
    // then recomposed once from the event loop, at the same two points and with the same guards as the 7.2c cleanup.
    void RunPendingRateReapply(const char* where);
    void ResetPerFileDiagnostics();
    void RecoverShaderCompileFailure(const std::string& line);
    void RecoverDeviceLoss();
    bool StartLogWriter();
    void LogWriterMain();
    void StopLogWriter();
    void ResolveAiBackend(const Json& video_config);
    // One generated VapourSynth stage before it is written: the substituted template (a complete single-stage
    // script with # --- header/body/footer --- sections) and what names its cache file.
    struct VsStageScript {
        std::string script;
        std::string signature;     // cache-key parameters (backend, model, heights, threads, ...)
        std::string template_name; // e.g. uai_trt.vpy
        std::string backend;       // dml | trt | cpu (MVTools)
    };
    bool PrepareUpscaleScript(const EnhancementPlan& plan, VsStageScript* out, std::string* error);
    bool PrepareInterpolationScript(const Json& settings, const std::string& mode,
                                    double source_fps, double display_fps,
                                    VsStageScript* out, std::string* error,
                                    double* chosen_factor, int* fps_num, int* fps_den,
                                    bool after_vs_upscale, int display_cap_height,
                                    int* resolved_pre_height, bool* pre_height_auto);
    // Writes data/cache/vs/<prefix>_<hash>.vpy (schema-versioned) and returns the vapoursynth= filter for it.
    bool WriteVsScript(const char* prefix, const std::string& signature, const std::string& script,
                       std::string* filter, std::string* error);
    void RecordShaderCompileMeasurement();
    void PresentFirstFrame(const char* trigger);
    void OnFirstFrameTimeout();
    void ApplyFileEnhancement(bool preload);
    bool SetOptionIfChanged(const char* name, const std::string& value);
    // Resolves the audio/sync policy from the current state and writes only video-sync / interpolation (and, on a
    // bit-perfect change, volume). source_fps_hint: the fps ApplyEnhancement used (demux-fps in on_preloaded), or
    // <= 0 to read container-fps. Logs one `audio-policy ...` line whenever the result changes.
    AudioSyncPolicy ApplyAudioSyncPolicy(const char* trigger, double source_fps_hint = 0.0);
    // Bit-perfect runtime check for the badge (audio_out.bitperfect / bitperfect_reason); sync reads only.
    std::string BitPerfectReason(bool* ok);
    // Actual resampling in the audio chain, from runtime properties: none | format | rate | speed | leftover, joined
    // with '+' (format = sample-format conversion only, e.g. shared-mode s32->float; speed = display-resample's
    // audio-speed-correction; leftover = the display-sync resampler mpv keeps after display-resample). Sync reads.
    std::string AudioResamplerState();
    void OnAudioOutputFailure(const std::string& line);
    double EffectiveDisplayHz();
    std::string BuildOpenedJson();
    std::string BuildTrackListJson(const char* wanted_type, const char* array_name);
    std::string BuildChaptersJson();
    std::string BuildAudioInfoJson();
    std::string BuildMediaInfoJson();
    // HDR output (video.hdrOutput) -> target-colorspace-hint, and the per-file HDR-source options (both branches, so
    // nothing stays set from an earlier HDR file). source: unknown | sdr | pq | hlg | hdr (see HdrSourceClass).
    void ApplyHdrOptions(const char* trigger, const std::string& source, const std::string& primaries);
    // frameProcessing.hdr / opened.media.hdr (sync reads: call only while SyncAllowed()).
    std::string HdrStateJson();
    void RefreshDisplayCaps();
    std::string AudioOutputJson();
    std::string audio_out_device_id_ = "\x01"; // forces the first lookup
    std::string audio_out_device_desc_;
    std::int64_t PositionMs();
    void AppendLog(const std::string& line);

    HMODULE library_{};
    mpv_handle* ctx_{};
    HWND host_{};
    HWND video_child_{};
    std::filesystem::path log_path_;
    std::filesystem::path host_log_path_;
    std::filesystem::path data_dir_;
    std::filesystem::path exe_dir_;
    std::filesystem::path enhancement_root_;
    std::string vs_script_; // active generated VapourSynth scripts, empty when none
    std::string round6_upscale_mode_;
    std::string round6_upscale_model_;
    std::string round6_upscale_label_;
    std::string round6_upscale_backend_;
    std::string round6_interpolation_mode_;
    std::string round6_interpolation_label_;
    std::string round6_interpolation_reason_; // why the selected interpolation is not running (amf_not_amd, amf_needs_d3d11, amf_failed, vs_interpolation_failed, vs_chain_failed)
    std::string round6_upscale_reason_;       // why the selected VS upscale is not running (vs_upscale_failed, vs_chain_failed)
    // VapourSynth stage failures detected from mpv's log (script evaluation error, frame error, VSScript load error).
    // vs_up_script_ / vs_fi_script_: file names of the generated scripts in the current chain (empty when none).
    // vs_failed_scripts_: scripts that failed for the current file; ApplyEnhancement keeps their stage inactive.
    // With upscale + interpolation in one generated script (chain_<hash>.vpy), vs_chain_script_ names it and
    // vs_up_script_ / vs_fi_script_ both name it too; a failure of that script is reported as vs_chain_failed.
    std::string vs_up_script_;
    std::string vs_fi_script_;
    std::string vs_chain_script_;
    bool vs_cache_checked_{};                 // stale generated scripts (older schema) removed once per process
    std::string vs_upscale_error_;            // first line of the captured error, for frameProcessing
    std::string vs_interpolation_error_;
    std::set<std::string> vs_failed_scripts_;
    // Audit 7.2c (per file): all VS stages failed and their dead filter is still in vf (cleanup pending); how many
    // cleanups ran (capped); a pass removed failed stages from vf (frameProcessing.vs_failed_removed).
    bool vs_dead_cleanup_pending_{};
    int vs_dead_cleanups_{};
    bool vs_dead_removed_{};
    std::vector<std::string> vs_chain_order_;  // stage per vf entry in order (upscale, interpolation, other)
    std::set<std::string> vs_failure_notified_; // per-file toast dedupe (stage|first line)
    bool vs_capture_active_{};                // inside a "Script evaluation failed" / "Filter error" block
    std::string vs_capture_stage_;            // upscale | interpolation | chain | all | "" (not mapped yet)
    std::string vs_capture_detail_;
    std::string vs_capture_source_;           // script | frame | runtime
    // Delivered-rate figures for frameProcessing and the periodic sync trace: one sample per frameProcessing tick
    // (<= 1 Hz) while the playback clock runs; every rate is taken over the same >= 5 s window (front .. back).
    struct RateSample {
        std::chrono::steady_clock::time_point at;
        double pos{};      // time-pos
        double drops{};    // frame-drop-count
        double avsync{};   // avsync (s)
        double vf_fps{};   // estimated-vf-fps at this sample
        double frames{};   // running integral of estimated-vf-fps d(time-pos) since the window was reset
    };
    std::deque<RateSample> rate_samples_;
    std::string rate_reset_reason_{"start"};  // last discontinuity (printed while the window is not yet valid)
    double rate_window_s_{-1.0};              // < 0: no valid window
    double rate_delivered_fps_{-1.0};         // estimate: integral(estimated-vf-fps d time-pos) / dt
    double rate_avsync_slope_{};              // d(avsync)/dt, s/s (positive = video falls further behind audio)
    double rate_drops_delta_{-1.0};           // frame-drop-count delta over the window
    double sync_drops_per_s_{-1.0};           // < 0: unknown
    double sync_presented_fps_{-1.0};         // estimate: delivered - drops/s (libmpv has no presented-frame counter)
    std::chrono::steady_clock::time_point sync_trace_at_{};
    int round6_upscale_scale_{1};
    int round6_upscale_pre_height_{1080};     // resolved upscale H_Pre (0 = no pre-scale)
    bool round6_upscale_pre_auto_{};          // upscaleParams.preHeight is auto
    int round6_upscale_pre_source_height_{};  // source height the auto rule used (0 = unknown at apply time)
    int round6_upscale_pre_hmax_{};           // H_Max the auto rule used (maxHeight or the monitor height)
    int round6_upscale_max_height_{};
    double round6_interpolation_factor_{};
    int round6_interpolation_fps_num_{1};
    int round6_interpolation_fps_den_{1};
    int round6_interpolation_pre_height_{}; // resolved VS interpolation H_PRE (0 = no shrink)
    bool round6_interpolation_pre_auto_{};
    bool round6_upscale_active_{};
    bool round6_interpolation_active_{};
    bool round6_migration_logged_{};
    bool applying_round6_enhancement_{};
    double round6_last_auto_source_fps_{};
    double round6_last_auto_display_hz_{};
    WakeFn wake_;
    EventFn event_;
    std::string current_path_;
    std::string current_audio_device_;
    std::string active_adapter_;
    std::uint32_t gpu_device_id_{};   // DXGI hardware-adapter index (DirectML device_id)
    std::uint32_t cuda_device_id_{};  // NVIDIA-only ordinal for TensorRT, derived from DXGI order (heuristic)
    std::string gpu_api_ = "d3d11";   // renderer API fixed at Initialize (a change needs a restart)
    std::string base_hwdec_ = "d3d11va"; // configured hwdec resolved for gpu_api_
    std::string ai_backend_ = "dml";  // resolved VapourSynth inference backend: dml | trt
    std::string ai_backend_reason_;
    bool trt_installed_{};
    bool trt_fallback_notified_{};
    std::string gpu_vendor_ = "any";
    std::string audio_exclusive_ = "yes";
    std::string audio_channels_ = "stereo";
    int audio_volume_{100};          // config audio.volume: the user's level (kept while bit-perfect forces 100)
    bool audio_mute_{};              // config audio.mute
    std::string master_clock_ = "auto"; // audio.masterClock (normalised): audio | auto
    std::string audio_policy_logged_; // last audio-policy line (logged on change only)
    std::string resampler_logged_;    // last audio-resampler line (logged on change only)
    std::string audio_policy_reason_; // reason of the last resolved policy (frameProcessing audio_out.policy_reason)
    double policy_source_fps_{};     // source fps of the last ApplyEnhancement (demux-fps during on_preloaded)
    std::string bitperfect_logged_;  // last bitperfect-check result (logged on change only)
    std::chrono::steady_clock::time_point exclusive_requested_at_{}; // OFF->ON transition in flight (failure reverts)
    std::string last_ao_error_;      // last ao/wasapi error line (detail for the exclusive-failure toast)
    std::string last_aid_;           // last selected audio track id (re-selected after an AO failure revert)
    // A display-* video-sync ran on this file's audio chain. mpv then keeps a resampler in the chain (its autoconvert
    // re-inserts swresample s32->s32 even after the sync returns to audio, measured); only a new audio chain drops it.
    bool display_sync_used_{};
    std::string seek_pref_ = "exact"; // playback.seekMode
    // HDR output state (ApplyHdrOptions / HdrStateJson).
    std::string hdr_output_setting_ = "auto"; // video.hdrOutput: auto | off | on
    std::string hdr_source_ = "unknown";      // source class the HDR options were last applied for
    std::string hdr_source_primaries_;
    bool hdr_ai_note_logged_{};               // per file: the BT.709-matrix parity note was logged
    struct DisplayCaps {
        HMONITOR monitor{};
        bool known{};          // DXGI IDXGIOutput6::GetDesc1 found the output
        bool hdr_enabled{};    // desktop colour space RGB_FULL_G2084_NONE_P2020 (Windows HDR on)
        bool capable_known{};  // DisplayConfigGetDeviceInfo answered
        bool hdr_capable{};    // HDR supported (ADVANCED_COLOR_INFO_2), else advancedColorSupported
        double max_nits{};     // DXGI_OUTPUT_DESC1::MaxLuminance
        std::string device;    // GDI device name, e.g. \\.\DISPLAY1
        std::string colorspace;
    };
    DisplayCaps display_caps_;
    std::string display_caps_logged_;
    bool display_caps_dirty_{true};
    std::chrono::steady_clock::time_point display_caps_at_{};
    std::string error_code_;
    std::string init_failure_;
    std::string last_state_;
    std::string last_nonfatal_error_; // code|message of the last non-fatal error this file
    std::chrono::steady_clock::time_point last_position_emit_{};
    int device_loss_retries_{};
    bool handling_device_loss_{};
    bool shader_failure_recovered_{};
    bool file_loaded_{};
    bool ended_emitted_{};
    // hwdec-current value last reported in an opened event ("" = still pending:
    // libmpv reports nothing until the video decoder exists, then its name or "no").
    std::string opened_hwdec_;
    bool lan_stream_{};
    bool device_loss_original_pending_{};
    // Surface handshake (see ui/app.js header): the child is shown at the first presented frame.
    bool pending_open_{};          // Open() issued, new file not loaded yet: END_FILE of the old one is a switch
    bool first_frame_pending_{};   // FILE_LOADED seen, PLAYBACK_RESTART not yet: child still hidden
    bool stop_requested_{};        // Stop() issued: END_FILE(stop) keeps the child visible
    bool preloading_{};            // inside the on_preloaded hook: ApplyEnhancement may run before FILE_LOADED
    bool hooks_available_{};
    std::int64_t open_start_ms_{};  // start position of the current Open()
    std::chrono::steady_clock::time_point open_requested_at_{};
    // Last value this backend set for each structural option (vf, glsl-shaders, hwdec, scalers,...).
    // They are global mpv options, so the record stays valid across files; anything that sets them
    // behind ApplyEnhancement's back clears it.
    std::map<std::string, std::string> applied_options_;
    bool initialize_attempted_{};
    bool initialized_{};
    // Core-reliability 1A: last adapter preflight (one DXGI snapshot per Initialize attempt).
    std::vector<gpu::AdapterInfo> adapters_;
    gpu::SavedSelection saved_adapter_;
    gpu::Resolution adapter_resolution_;
    bool adapter_preflight_done_{};
    bool adapter_preflight_blocked_{};
    bool adapter_persist_pending_{};
    std::string last_preflight_line_;
    Json video_config_;
    EnhancementCompiler enhancement_compiler_;
    EnhancementPlan enhancement_plan_;
    bool enhancement_configured_{};
    std::chrono::steady_clock::time_point last_frame_processing_emit_{};
    // frameProcessing snapshot (see EmitFrameProcessing).
    std::map<std::string, FpSnapshotValue> fp_snapshot_;  // key: kind ('S' | 'D' | 'N') + property name
    std::map<std::uint64_t, std::string> fp_requests_;    // outstanding reply id -> key
    std::uint64_t fp_generation_{};
    std::uint64_t fp_epoch_{};          // bumped by BeginVsBusy and START_FILE
    std::uint64_t fp_request_epoch_{};
    int fp_phase_{};                    // 0 idle | 1 main batch | 2 audio-device-list (selected device changed)
    std::chrono::steady_clock::time_point fp_requested_at_{};
    bool fp_snapshot_active_{};         // BuildFrameProcessing is running: reads come from the snapshot
    std::set<std::string> fp_snapshot_misses_;
    std::chrono::steady_clock::time_point pending_shader_compile_start_{};
    std::string pending_shader_compile_ids_;
    std::set<std::string> measured_shader_ids_;
    std::atomic<bool> wake_pending_{};
    // UI-thread stall guard state (see SyncAllowed).
    struct CachedProperty {
        std::string text;   // as mpv_get_property_string would format it (flags: yes/no)
        double number{};
        bool has_number{};
    };
    std::map<std::string, CachedProperty> observed_cache_; // last MPV_EVENT_PROPERTY_CHANGE payload per observed name
    bool vs_busy_{};
    std::string vs_busy_trigger_;                        // load | apply | seek
    std::chrono::steady_clock::time_point vs_busy_since_{};
    bool vs_busy_timeout_reported_{};
    bool vs_busy_stop_requested_{};
    std::uint64_t vs_probe_id_{};                         // outstanding busy probe (apply readiness or barrier)
    int vs_probe_kind_{};                                 // 0 none | 1 apply readiness | 2 barrier step 1 | 3 barrier step 2 | 4 reconfig watch
    std::chrono::steady_clock::time_point vs_probe_sent_{};
    std::string vs_barrier_by_;                           // end condition the running barrier will report
    std::chrono::steady_clock::time_point vs_barrier_since_{};
    std::string vs_end_pending_by_;                       // end condition met inside the PollEvents drain
    int vs_seek_cmds_{};                                  // seek commands issued during this busy period
    int vs_seek_events_{};                                // MPV_EVENT_SEEK seen during this busy period
    bool vs_restart_pending_{};                           // a seek executed (MPV_EVENT_SEEK) and its PLAYBACK_RESTART not seen yet
    bool polling_{};                                      // inside PollEvents' event drain
    // Audit 7.3b: last complete video-params signature of this file (w x h pixelformat matrix levels primaries gamma).
    std::string reconfig_sig_;
    int reconfig_watch_probes_{};                         // watch probes answered in this reconfig period
    double reconfig_watch_max_ms_{};                      // longest watch-probe wait in this reconfig period
    bool vs_deferred_opened_{};
    bool vs_deferred_subtitle_{};
    bool vs_deferred_file_enhancement_{};
    bool vs_deferred_apply_{};
    Json vs_deferred_config_;
    std::string wasapi_devices_cache_ = "[]";
    std::string last_frame_processing_json_; // diagnostics: last frameProcessing / opened payloads (see Emit)
    std::string last_opened_json_;
    std::string mpv_version_;
    std::string ffmpeg_version_;
    std::uint64_t reply_seq_{};
    std::map<std::uint64_t, std::string> pending_replies_; // reply id -> what was sent (for error logs)
    bool shutdown_detached_{};                             // terminate_destroy still running on its worker
    // --- Core-reliability 1B: libmpv wakeup delivery (report 25-B) ---------------------------------------------------
    // libmpv calls the wakeup callback once per drain-to-NONE cycle (its own need_wakeup latch), so a failed
    // PostMessage (e.g. ERROR_NOT_ENOUGH_QUOTA while the UI thread is stalled) used to stop all event processing for
    // good. Invariant: at most one wake token is outstanding (wake_pending_ == true): a posted WM_APP_MPV_WAKE, or the
    // retry timer armed after a failed post. Only PollEvents releases the token. The callback and the retry timer touch
    // atomics only; PollEvents logs one line after a recovery.
    static void OnMpvWakeup(void* opaque);
    static void CALLBACK OnWakeRetry(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER timer);
    void DeliverWake();                 // mpv thread or threadpool thread
    void CloseWakeRetry();              // UI thread (BeginShutdown): after the callback is removed
    std::atomic<bool> wake_stopping_{};
    std::atomic<std::uint32_t> wake_post_failures_{};     // failed posts since the last drain
    std::atomic<std::uint32_t> wake_last_error_{};
    std::atomic<std::uint32_t> wake_retry_ms_{};          // current backoff (16 ms doubling to 1 s); 0 = healthy
    std::atomic<std::uint32_t> wake_retries_{};           // retry-timer posts since the last drain
    std::atomic<std::int64_t> wake_failed_since_ns_{};    // steady clock of the first failure since the last drain
    PTP_TIMER wake_retry_timer_{};
    bool wake_watchdog_{};                                // degraded mode (no threadpool timer): 1 s UI timer
    // --- Core-reliability 2B: async loadfile correlation (report 25-C section 2) -------------------------------------
    // One request per Open() / device-loss reload. The loadfile COMMAND_REPLY carries the playlist_entry_id that the
    // following START_FILE / END_FILE of that entry carry, so events of an older entry (replaced or superseded) are told
    // apart from the current one. A failure is reported once (FailLoad) and latched: EmitState keeps "error" until the
    // next Open / Stop / reload, so idle-active / core-idle changes after it no longer turn the UI to "stopped".
    struct LoadRequest {
        std::uint64_t gen{};            // 0 = no active request
        std::int64_t entry_id{};        // from the reply; 0 = reply not seen, -1 = reply without an id (adopt next START)
        bool reply_seen{};
        bool started{};
        const char* origin{"open"};     // open | device-loss
        std::chrono::steady_clock::time_point at{};
    };
    LoadRequest load_;
    std::uint64_t load_gen_{};
    std::map<std::uint64_t, std::uint64_t> load_replies_; // loadfile reply id -> request generation
    bool load_error_latched_{};
    void BeginLoadRequest(std::uint64_t reply_id, const char* origin);
    void OnLoadfileReply(std::uint64_t id, int error, const void* data);
    void FailLoad(const char* code, const std::string& message, int mpv_error, const char* why, bool stop_player);
    std::string LoadErrorText(int mpv_error) const;
    // --- Core-reliability 3A (see RunPendingRateReapply) --------------------------------------------------------------
    bool deint_seen_{};
    bool vs_fps_limit_seen_{};
    bool vs_rate_reapply_pending_{};
    std::string deint_active_logged_;   // last deinterlace-active value logged for this file
    std::shared_ptr<void> shutdown_done_;                  // event HANDLE set by the terminate_destroy worker
    std::chrono::steady_clock::time_point shutdown_started_{};
    std::mutex log_mutex_;
    std::condition_variable log_cv_;
    std::deque<std::string> log_queue_;
    std::thread log_thread_;
    bool log_writer_started_{};
    bool log_writer_stop_{};
    std::vector<HMODULE> dependencies_;
    std::vector<void*> dll_directories_;

    struct Api;
    std::unique_ptr<Api> api_;
};

} // namespace vp
