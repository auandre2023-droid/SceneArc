#include "enhancement_compiler.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace vp {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool SafeRelative(const std::string& path) {
    if (path.empty() || std::filesystem::path(path).is_absolute()) return false;
    std::filesystem::path parsed(path);
    for (const auto& part : parsed) if (part == "..") return false;
    return true;
}

} // namespace

bool EnhancementCompiler::Load(const std::filesystem::path& catalog_path, std::string* error) {
    catalog_json_.clear();
    catalog_ = Json{};
    std::ifstream input(catalog_path, std::ios::binary);
    if (!input) {
        if (error) *error = "MPV enhancement catalog is missing: " + catalog_path.string();
        return false;
    }
    std::ostringstream content;
    content << input.rdbuf();
    catalog_json_ = content.str();
    bool valid = false;
    catalog_ = Json::Parse(catalog_json_, &valid);
    if (!valid || !catalog_.is_object() || !catalog_["entries"].is_array()) {
        catalog_json_.clear();
        if (error) *error = "MPV enhancement catalog is not valid JSON or has no entries array.";
        return false;
    }
    return true;
}

const Json* EnhancementCompiler::FindEntry(const std::string& id) const {
    for (const Json& entry : catalog_["entries"].items())
        if (entry["id"].str() == id) return &entry;
    return nullptr;
}

const Json* EnhancementCompiler::FindPreset(const std::string& id) const {
    for (const Json& preset : catalog_["quickPresets"].items())
        if (preset["id"].str() == id) return &preset;
    return nullptr;
}

bool EnhancementCompiler::ResolveItem(const std::string& id, const std::string& stage, const std::string& api,
                                      const std::string& vendor, const std::string& hwdec,
                                      const std::filesystem::path& asset_root,
                                      const std::filesystem::path& vs_runtime_root, const std::string& custom_ai_model,
                                      bool enforce_budget, std::vector<std::string>* paths, double* cost_ms, double* compile_ms,
                                      std::string* ai_model_file,
                                      std::string* error) const {
    if (paths) paths->clear();
    if (cost_ms) *cost_ms = 0.0;
    if (compile_ms) *compile_ms = 0.0;
    if (ai_model_file) ai_model_file->clear();
    if (id.empty() || id == "off") return true;
    const Json* entry = FindEntry(id);
    if (!entry || entry->operator[]("stage").str() != stage) {
        if (error) *error = "Unknown " + stage + " enhancement: " + id;
        return false;
    }
    const std::string wanted_vendor = entry->operator[]("vendor").str("any");
    if (wanted_vendor != "any" && Lower(wanted_vendor) != Lower(vendor)) {
        if (error) *error = id + " requires a " + wanted_vendor + " GPU.";
        return false;
    }
    if (entry->operator[]("engine").str() == "vs-dml") {
        std::string model = entry->operator[]("model").str();
        if (id == "uai_custom") model = custom_ai_model;
        const std::filesystem::path model_path(model);
        if (model.empty() || model_path.filename() != model_path || Lower(model_path.extension().string()) != ".onnx") {
            if (error) *error = id + " requires a local .onnx model filename.";
            return false;
        }
        const auto model_on_disk = vs_runtime_root / L"vs-plugins" / L"models" / model_path;
        if (!std::filesystem::is_regular_file(model_on_disk)) {
            if (error) *error = "The DirectML model is unavailable in the local VapourSynth runtime.";
            return false;
        }
        double measured_fps = entry->operator[]("throughputFps").num(0.0);
        if (id == "uai_custom") {
            for (const Json& measured_entry : catalog_["entries"].items()) {
                if (measured_entry["engine"].str() == "vs-dml" && measured_entry["model"].str() == model) {
                    measured_fps = measured_entry["throughputFps"].num(0.0);
                    if (measured_fps > 0.0) break;
                }
            }
        }
        if (measured_fps <= 0.0) {
            if (enforce_budget) {
                if (error) *error = id + " has no measured real-time throughput and is disabled until measured.";
                return false;
            }
            // Budget guard off: allow the unmeasured model; its cost is unknown, so it adds nothing to the estimate.
            if (compile_ms) *compile_ms = entry->operator[]("compileMs").num(0.0);
            if (ai_model_file) *ai_model_file = std::move(model);
            return true;
        }
        if (cost_ms) *cost_ms = 1000.0 / measured_fps;
        if (compile_ms) *compile_ms = entry->operator[]("compileMs").num(0.0);
        if (ai_model_file) *ai_model_file = std::move(model);
        return true;
    }
    const double measured_cost = entry->operator[]("costMs").num(-1.0);
    if (measured_cost < 0) {
        if (error) *error = id + " has no measured GPU pass budget and is disabled until measured.";
        return false;
    }
    if (cost_ms) *cost_ms = measured_cost;
    if (compile_ms) *compile_ms = entry->operator[]("compileMs").num(0.0);
    if (entry->operator[]("kind").str() == "vf") {
        if (entry->operator[]("requiresHwdec").str() == "d3d11va" && hwdec != "d3d11va") {
            if (error) *error = id + " requires D3D11 hardware decoding.";
            return false;
        }
        return true;
    }
    const Json& api_files = entry->operator[]("files")[api];
    if (!api_files.is_array() || api_files.items().empty()) {
        if (error) *error = id + " has no shader variant for " + api + ".";
        return false;
    }
    for (const Json& file : api_files.items()) {
        const std::string relative = file.str();
        if (!SafeRelative(relative)) {
            if (error) *error = "Unsafe shader path in catalog entry " + id + ".";
            return false;
        }
        const std::filesystem::path full = asset_root / std::filesystem::path(relative);
        if (!std::filesystem::is_regular_file(full)) {
            if (error) *error = "Shader asset is missing for " + id + ": " + relative;
            return false;
        }
        if (paths) {
            const auto utf8 = full.u8string();
            paths->emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }
    }
    return true;
}

bool EnhancementCompiler::Compile(const Json& config, const std::string& gpu_api, const std::string& gpu_vendor,
                                  const std::filesystem::path& asset_root, const std::filesystem::path& vs_runtime_root,
                                  double source_fps, double display_fps, double speed, const std::string& hwdec,
                                  double average_kbps, bool hdr_source,
                                  EnhancementPlan* output, std::string* error) const {
    if (!output || catalog_json_.empty()) {
        if (error) *error = "MPV enhancement catalog is not loaded.";
        return false;
    }
    EnhancementPlan plan;
    plan.source_fps = source_fps;
    plan.output_fps = display_fps;
    // HDR source (PQ / HLG transfer, or a tagged peak above SDR white): MpvBackend HdrSourceClass. Never the bit depth.
    plan.hdr_active = hdr_source;
    plan.preset = config["preset"].str("original");
    plan.restore = config["restore"].str("off");
    plan.upscale = config["upscaler"].str(config["upscale"].str("off"));
    plan.sharpen = config["sharpen"].str("off");
    plan.deband = config["deband"].str("off");
    plan.smooth = config["smooth"].str("off");
    plan.ai_pre_height = static_cast<int>(config["ai"]["preHeight"].num(720));
    plan.ai_max_height = static_cast<int>(config["ai"]["maxHeight"].num(0));
    plan.ai_gpu_threads = std::clamp(static_cast<int>(config["ai"]["gpuThreads"].num(2)), 1, 3);
    plan.ai_fp16 = config["ai"]["fp16"].boolean(true);
    // GPU budget guard (off unless enabled): when off, every selected module runs as chosen and an
    // over-budget combination is only reported (reason "over_budget"), never replaced or switched off.
    const bool enforce_budget = config["gpuBudgetGuard"].boolean(false);
    if (plan.ai_pre_height != 0 && plan.ai_pre_height != 720 && plan.ai_pre_height != 1080) plan.ai_pre_height = 720;
    if (plan.ai_max_height < 0) plan.ai_max_height = 0;

    const Json* preset = plan.preset == "custom" ? nullptr : FindPreset(plan.preset);
    if (plan.preset != "custom" && !preset) {
        if (error) *error = "Unknown MPV quick preset: " + plan.preset;
        return false;
    }
    if (preset) {
        plan.restore = (*preset)["restore"].str("off");
        plan.upscale = (*preset)["upscale"].str("off");
        plan.sharpen = (*preset)["sharpen"].str("off");
        const double max_fps = (*preset)["maxFps"].num(0);
        if (max_fps > 0.0 && source_fps > max_fps && enforce_budget && !(*preset)["fallback"].is_null()) {
            const std::string fallback = (*preset)["fallback"]["upscale"].str();
            if (!fallback.empty()) {
                plan.upscale = fallback;
                // This guard is the preset's GPU budget fallback: extreme shaders
                // are deliberately disabled above their supported source rate.
                plan.reason = "gpu_budget";
            }
        }
    }

    // (No 8-bit / non-HDR gate for catalog "vs-dml" entries any more: MpvBackend never passes one here -- the upscaler
    // is a GLSL id or "off"; AI upscaling has its own VapourSynth path -- and 10-bit SDR is processed like 8-bit, as in
    // mpv-lazy.)

    std::vector<std::string> restore_paths, upscale_paths, sharpen_paths;
    double restore_cost = 0.0, upscale_cost = 0.0, sharpen_cost = 0.0;
    double restore_compile_ms = 0.0, upscale_compile_ms = 0.0, sharpen_compile_ms = 0.0;
    std::string restore_ai_model, upscale_ai_model, sharpen_ai_model;
    const std::string custom_ai_model = config["ai"]["modelFile"].str();
    std::string resolve_error;
    if (!ResolveItem(plan.restore, "restore", gpu_api, gpu_vendor, hwdec, asset_root, vs_runtime_root, custom_ai_model, enforce_budget,
                     &restore_paths, &restore_cost, &restore_compile_ms, &restore_ai_model, &resolve_error)) {
        if (error) *error = resolve_error;
        return false;
    }
    if (!ResolveItem(plan.upscale, "upscale", gpu_api, gpu_vendor, hwdec, asset_root, vs_runtime_root, custom_ai_model, enforce_budget,
                     &upscale_paths, &upscale_cost, &upscale_compile_ms, &upscale_ai_model, &resolve_error)) {
        const std::string fallback = preset ? (*preset)["fallback"]["upscale"].str() : std::string();
        if (fallback.empty() || fallback == plan.upscale) {
            if (error) *error = resolve_error;
            return false;
        }
        plan.upscale = fallback;
        plan.reason = "ai_unavailable";
        resolve_error.clear();
        if (!ResolveItem(plan.upscale, "upscale", gpu_api, gpu_vendor, hwdec, asset_root, vs_runtime_root, custom_ai_model, enforce_budget,
                         &upscale_paths, &upscale_cost, &upscale_compile_ms, &upscale_ai_model, &resolve_error)) {
            if (error) *error = resolve_error;
            return false;
        }
    }
    if (!ResolveItem(plan.sharpen, "sharpen", gpu_api, gpu_vendor, hwdec, asset_root, vs_runtime_root, custom_ai_model, enforce_budget,
                     &sharpen_paths, &sharpen_cost, &sharpen_compile_ms, &sharpen_ai_model, &resolve_error)) {
        if (error) *error = resolve_error;
        return false;
    }

    if (source_fps > 0.0) {
        const double limit_ms = 0.8 * (1000.0 / source_fps);
        const double estimated = restore_cost + upscale_cost + sharpen_cost;
        if (estimated > limit_ms && enforce_budget) {
            const std::string fallback = preset ? (*preset)["fallback"]["upscale"].str() : std::string();
            if (!fallback.empty() && fallback != plan.upscale) {
                plan.upscale = fallback;
                plan.reason = "gpu_budget";
                upscale_paths.clear();
                if (!ResolveItem(plan.upscale, "upscale", gpu_api, gpu_vendor, hwdec, asset_root, vs_runtime_root, custom_ai_model, enforce_budget,
                                 &upscale_paths, &upscale_cost, &upscale_compile_ms, &upscale_ai_model, &resolve_error)) {
                    if (error) *error = resolve_error;
                    return false;
                }
                if (restore_cost + upscale_cost + sharpen_cost > limit_ms) {
                    plan.restore = "off";
                    plan.upscale = "off";
                    plan.sharpen = "off";
                    restore_paths.clear();
                    upscale_paths.clear();
                    upscale_ai_model.clear();
                    upscale_compile_ms = 0.0;
                    sharpen_paths.clear();
                    restore_cost = upscale_cost = sharpen_cost = 0.0;
                }
            } else {
                plan.restore = "off";
                plan.upscale = "off";
                plan.sharpen = "off";
                restore_paths.clear();
                upscale_paths.clear();
                upscale_ai_model.clear();
                upscale_compile_ms = 0.0;
                sharpen_paths.clear();
                restore_cost = upscale_cost = sharpen_cost = 0.0;
                plan.reason = "gpu_budget";
            }
        }
    }

    const Json* upscale_entry = plan.upscale == "off" ? nullptr : FindEntry(plan.upscale);
    const Json* restore_entry = plan.restore == "off" ? nullptr : FindEntry(plan.restore);
    const Json* sharpen_entry = plan.sharpen == "off" ? nullptr : FindEntry(plan.sharpen);
    auto append = [&](const std::string& id, const Json* entry, const std::vector<std::string>& paths) {
        if (!entry) return;
        if (entry->operator[]("engine").str() == "vs-dml") {
            plan.ai_dml = true;
            plan.ai_model_file = id == "uai_custom" ? custom_ai_model : entry->operator[]("model").str();
            plan.filter = "vapoursynth";
        } else if (entry->operator[]("kind").str() == "vf") plan.filter = entry->operator[]("filter").str();
        else {
            plan.shader_ids.push_back(id);
            plan.shader_paths.insert(plan.shader_paths.end(), paths.begin(), paths.end());
        }
    };
    append(plan.restore, restore_entry, restore_paths);
    append(plan.upscale, upscale_entry, upscale_paths);
    append(plan.sharpen, sharpen_entry, sharpen_paths);
    if (upscale_entry) plan.upscale_scale = static_cast<int>(upscale_entry->operator[]("scale").num(1));
    plan.estimated_cost_ms = restore_cost + upscale_cost + sharpen_cost;
    plan.estimated_compile_ms = restore_compile_ms + upscale_compile_ms + sharpen_compile_ms;
    if (plan.ai_dml) plan.ai_model_file = upscale_ai_model;

    if (plan.upscale == "nv_rtx_vsr" && (plan.restore != "off" || plan.sharpen != "off")) {
        if (error) *error = "NVIDIA RTX VSR is exclusive and cannot be combined with another shader stage.";
        return false;
    }

    const std::string smooth_mode = plan.smooth;
    if (smooth_mode == "on") plan.smooth_active = true;
    else if (smooth_mode == "auto") {
        if (speed != 1.0) plan.reason = plan.reason.empty() ? "speed_not_one" : plan.reason;
        else if (source_fps > 32.0) plan.reason = plan.reason.empty() ? "fps_gt_32" : plan.reason;
        else if (source_fps > 0.0 && display_fps > 0.0 && display_fps / source_fps > 3.2)
            plan.reason = plan.reason.empty() ? "refresh_ratio" : plan.reason;
        else plan.smooth_active = true;
    }
    plan.deband_active = plan.deband == "on" || (plan.deband == "auto" && average_kbps > 0 && average_kbps <= 3000.0);
    plan.active = !plan.shader_paths.empty() || !plan.filter.empty() || plan.deband_active || plan.smooth_active || plan.hdr_active;
    *output = std::move(plan);
    return true;
}

} // namespace vp
