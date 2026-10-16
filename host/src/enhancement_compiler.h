#pragma once

#include "json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vp {

struct EnhancementPlan {
    std::string preset = "original";
    std::string restore = "off";
    std::string upscale = "off";
    std::string sharpen = "off";
    std::string deband = "off";
    std::string smooth = "off";
    std::string reason;
    std::string filter;
    std::string ai_model_file;
    std::vector<std::string> shader_ids;
    std::vector<std::string> shader_paths;
    double estimated_cost_ms{};
    double estimated_compile_ms{};
    double source_fps{};
    double output_fps{};
    int upscale_scale = 1;
    int ai_pre_height = 720;
    int ai_max_height{};
    int ai_gpu_threads = 2;
    bool ai_fp16 = true;
    bool ai_dml{};
    bool active{};
    bool deband_active{};
    bool smooth_active{};
    bool hdr_active{};
};

class EnhancementCompiler {
public:
    bool Load(const std::filesystem::path& catalog_path, std::string* error = nullptr);
    const std::string& CatalogJson() const { return catalog_json_; }
    bool Compile(const Json& config, const std::string& gpu_api, const std::string& gpu_vendor,
                 const std::filesystem::path& asset_root, const std::filesystem::path& vs_runtime_root,
                 double source_fps, double display_fps, double speed, const std::string& hwdec,
                 double average_kbps, bool hdr_source,
                 EnhancementPlan* output, std::string* error = nullptr) const;

private:
    const Json* FindEntry(const std::string& id) const;
    const Json* FindPreset(const std::string& id) const;
    bool ResolveItem(const std::string& id, const std::string& stage, const std::string& api,
                     const std::string& vendor, const std::string& hwdec, const std::filesystem::path& asset_root,
                     const std::filesystem::path& vs_runtime_root, const std::string& custom_ai_model, bool enforce_budget,
                     std::vector<std::string>* paths, double* cost_ms, double* compile_ms, std::string* ai_model_file,
                     std::string* error) const;

    std::string catalog_json_;
    Json catalog_;
};

} // namespace vp
