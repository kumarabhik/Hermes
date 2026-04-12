#pragma once

#include "hermes/core/types.hpp"

#include <string>

namespace hermes {

class LoadAvgMonitor {
public:
    LoadAvgMonitor() = default;

    // Reads /proc/loadavg and stores the runnable count as a Tier A fallback signal.
    bool update_sample(PressureSample& sample);

private:
    std::string loadavg_path_ = "/proc/loadavg";
};

} // namespace hermes
