#include "hermes/monitor/vmstat.hpp"

#include <fstream>
#include <sstream>

namespace hermes {

bool VmstatMonitor::update_sample(PressureSample& sample) {
    std::ifstream file(vmstat_path_);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    bool found_pgfault = false;
    bool found_pgmajfault = false;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t value = 0;
        iss >> key >> value;

        if (key == "pgfault") {
            sample.vmstat_pgfault = value;
            found_pgfault = true;
        } else if (key == "pgmajfault") {
            sample.vmstat_pgmajfault = value;
            found_pgmajfault = true;
        }

        if (found_pgfault && found_pgmajfault) {
            break;
        }
    }

    return found_pgfault || found_pgmajfault;
}

} // namespace hermes
