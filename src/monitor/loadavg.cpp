#include "hermes/monitor/loadavg.hpp"

#include <fstream>
#include <sstream>

namespace hermes {

bool LoadAvgMonitor::update_sample(PressureSample& sample) {
    std::ifstream file(loadavg_path_);
    if (!file.is_open()) {
        return false;
    }

    std::string avg1;
    std::string avg5;
    std::string avg15;
    std::string runnable_field;

    if (!(file >> avg1 >> avg5 >> avg15 >> runnable_field)) {
        return false;
    }

    const std::size_t slash_pos = runnable_field.find('/');
    if (slash_pos == std::string::npos) {
        return false;
    }

    try {
        sample.loadavg_runnable = static_cast<uint32_t>(std::stoul(runnable_field.substr(0, slash_pos)));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace hermes
