#pragma once

#include "hermes/core/types.hpp"

#include <string>

namespace hermes {

class CpuPsiMonitor {
public:
    CpuPsiMonitor() = default;
    
    // Reads /proc/pressure/cpu and updates the cpu fields in the provided sample
    // Returns true on success, false if the file is unavailable or parsing fails
    bool update_sample(PressureSample& sample);

private:
    std::string psi_path_ = "/proc/pressure/cpu";
};

} // namespace hermes
