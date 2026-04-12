#pragma once

#include "hermes/core/types.hpp"

#include <string>

namespace hermes {

class MemPsiMonitor {
public:
    MemPsiMonitor() = default;

    // Reads /proc/pressure/memory and updates the memory fields in the provided sample
    // Returns true on success, false if the file is unavailable or parsing fails
    bool update_sample(PressureSample& sample);

private:
    std::string psi_path_ = "/proc/pressure/memory";
};

} // namespace hermes
