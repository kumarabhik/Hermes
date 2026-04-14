#pragma once

#include "hermes/core/types.hpp"

#include <string>

namespace hermes {

// IoPsiMonitor reads /proc/pressure/io and populates io_some_avg10 and io_full_avg10
// in the provided PressureSample. Returns false on Tier A environments (WSL, Windows)
// where the file does not exist. IO PSI is a Tier B/C signal.
class IoPsiMonitor {
public:
    IoPsiMonitor() = default;

    bool update_sample(PressureSample& sample);

private:
    std::string psi_path_ = "/proc/pressure/io";
};

} // namespace hermes
