#pragma once

#include "hermes/core/types.hpp"

#include <string>
#include <vector>

namespace hermes {

class GpuStatsCollector {
public:
    GpuStatsCollector() = default;

    // Uses a lightweight nvidia-smi query path when available.
    bool update_sample(PressureSample& sample);
    std::vector<GpuProcessUsage> query_process_usage();

    const std::string& last_error() const {
        return last_error_;
    }

private:
    bool run_command(const std::string& command, std::string& output);

    std::string last_error_;
};

} // namespace hermes
