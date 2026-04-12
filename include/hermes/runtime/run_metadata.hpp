#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace hermes {

struct RunMetadataConfig {
    std::filesystem::path artifact_root{"artifacts"};
    std::filesystem::path run_directory;
    std::filesystem::path config_path{"config/schema.yaml"};
    std::string run_id;
    std::string scenario{"observe"};
    std::string config_hash{"unknown"};
    std::string runtime_mode{"observe-only"};
    uint64_t started_ts_wall{0};
    uint64_t started_ts_mono{0};
};

class RunMetadataWriter {
public:
    bool write(const RunMetadataConfig& config, std::string& error) const;

private:
    bool write_config_snapshot(const RunMetadataConfig& config, std::string& error) const;
    bool write_run_metadata(const RunMetadataConfig& config, std::string& error) const;
};

} // namespace hermes
