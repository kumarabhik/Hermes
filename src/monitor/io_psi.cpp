#include "hermes/monitor/io_psi.hpp"

#include <fstream>
#include <sstream>

namespace hermes {

bool IoPsiMonitor::update_sample(PressureSample& sample) {
    std::ifstream file(psi_path_);
    if (!file.is_open()) {
        // IO PSI is a Tier B/C signal. On WSL and Windows it will be absent.
        return false;
    }

    std::string line;
    bool found_some = false;
    bool found_full = false;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string type;
        iss >> type;

        if (type == "some") {
            std::string avg10_str;
            iss >> avg10_str;
            if (avg10_str.rfind("avg10=", 0) == 0) {
                try {
                    sample.io_some_avg10 = std::stod(avg10_str.substr(6));
                    found_some = true;
                } catch (...) {
                    return false;
                }
            }
        } else if (type == "full") {
            std::string avg10_str;
            iss >> avg10_str;
            if (avg10_str.rfind("avg10=", 0) == 0) {
                try {
                    sample.io_full_avg10 = std::stod(avg10_str.substr(6));
                    found_full = true;
                } catch (...) {
                    return false;
                }
            }
        }
    }

    return found_some || found_full;
}

} // namespace hermes
