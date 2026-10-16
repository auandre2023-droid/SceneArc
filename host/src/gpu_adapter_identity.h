// Core-reliability 1A (report 25-A §3): GPU adapter identity, preflight and reconciliation for the MPV edition.
//
// The MPV edition stores the user's GPU as video.gpuAdapterLuid (a decimal LUID; "" = automatic) plus, from this change
// on, video.gpuAdapterIdentity {v:1, vendorId, deviceId, subSysId, revision, name, dedicatedVideoMemory}. A LUID is only
// unique until the next restart and can be reused by another adapter, so it is checked against the identity, and a stale
// LUID is reconciled only by an exact, unique identity match. Everything except EnumerateAdapters is pure (no DXGI), so
// the acceptance harness tests it with synthetic adapter lists (MpvAcceptanceHarness --mode gpu-identity).
//
// libmpv selects the D3D11 adapter by description PREFIX (--d3d11-adapter: first adapter in DXGI order whose description
// starts with the string, ASCII case-insensitive). An explicitly chosen adapter that an earlier adapter's name shadows
// (identical twins, "RTX 4070" after "RTX 4070 Ti") cannot be addressed and is blocked instead of silently rendering on the
// other card.
#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vp::gpu {

struct AdapterInfo {
    std::uint64_t luid{};
    std::uint32_t vendor{}, device{}, subsys{}, revision{};
    std::string name;                 // DXGI description (UTF-8)
    std::uint64_t vram{};             // DedicatedVideoMemory (display only, never a match key)
    std::uint32_t dxgi_index{};       // EnumAdapters1 index (software adapters included): mpv's d3d11-adapter order
    std::uint32_t hardware_index{};   // index among hardware adapters = DML device id = getGpuAdapters "index"
    bool software{};
};

struct SavedSelection {
    std::string luid_text;            // video.gpuAdapterLuid, or the PlaybackCore top-level key when video has none
    std::string source = "video";     // "video" | "legacy_top_level"
    bool has_identity{};
    std::uint32_t vendor{}, device{}, subsys{}, revision{};
    std::string name;
};

enum class Outcome { Auto, Exact, ExactUnverified, Reconciled, NotFound, Ambiguous, NotAddressable, Invalid };

struct Resolution {
    Outcome outcome = Outcome::Auto;
    int adapter = -1;                 // index into the snapshot; Auto: the first hardware adapter (-1: none)
    std::vector<int> candidates;      // Ambiguous: the identical adapters
    int suggestion = -1;              // NotFound: a unique vendor+device match (UI hint only, never applied)
    int prefix_winner = -1;           // NotAddressable: the adapter mpv's prefix match would render on
    std::string reason;
};

// One DXGI pass in EnumAdapters1 order; adapters whose GetDesc1 fails are skipped (as before).
std::vector<AdapterInfo> EnumerateAdapters();
// video.gpuAdapterLuid, falling back to the top-level key only when video has no string member (MpvBackend before 1A).
SavedSelection ParseSavedSelection(const Json& config);
Resolution Reconcile(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, bool d3d11_render);
// Index of the adapter mpv picks for d3d11-adapter=name (-1: none).
int MpvD3d11PrefixWinner(const std::vector<AdapterInfo>& adapters, const std::string& name);
bool MpvAddressable(const std::vector<AdapterInfo>& adapters, int index);
// CUDA ordinal heuristic (unchanged): NVIDIA hardware adapters before hardware_index in DXGI order.
std::uint32_t CudaOrdinal(const std::vector<AdapterInfo>& adapters, std::uint32_t hardware_index);

const char* OutcomeName(Outcome outcome);
bool IsBlocking(Outcome outcome);
const char* ErrorCodeFor(Outcome outcome); // "" unless blocking
// Traditional Chinese text for the player error, the banner and init_failure_.
std::string UserMessage(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution);
std::string IdentityJson(const AdapterInfo& adapter);
// getGpuAdapters "adapters" (hardware only): index, luid, name, vendorId, deviceId, subSysId, revision,
// dedicatedVideoMemory, mpvAddressable.
std::string AdapterListJson(const std::vector<AdapterInfo>& adapters);
// gpuAdapterStatus payload. libmpv_attempted: the process-scoped libmpv attempt has been made (a GPU change now needs a restart).
std::string StatusJson(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution,
                       bool libmpv_attempted);
// mpv-host.log "gpu adapter preflight ..." line (without timing; equal lines mean an unchanged result).
std::string PreflightLogLine(const SavedSelection& saved, const std::vector<AdapterInfo>& adapters, const Resolution& resolution);

// Sets video.gpuAdapterLuid and video.gpuAdapterIdentity (empty identity_json: removes the identity) in an MPV view config
// text. The video object is rebuilt member by member and put in place of the top-level "video" value found structurally
// (mpv_config_safety's SetObjectMember would add a duplicate key for the string member, and its first-occurrence search
// can hit a string value "video", e.g. a library name); the result is verified: every other top-level member and every
// other video member must be unchanged, else false and nothing is written.
bool SetVideoGpuAdapterText(const std::string& view_text, const std::string& luid, const std::string& identity_json,
                            std::string* out);

} // namespace vp::gpu
