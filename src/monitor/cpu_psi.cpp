#include "hermes/monitor/cpu_psi.hpp"

#include <fstream>
#include <sstream>

namespace hermes {

bool CpuPsiMonitor::update_sample(PressureSample& sample) {
    std::ifstream file(psi_path_);
    if (!file.is_open()) {
        // Fallback for Tier A deployments (e.g., Windows/WSL without PSI)
        // Returning false cleanly prevents the daemon from aborting
        return false;
    }

    // Expected format:
    // some avg10=X.XX avg60=Y.YY avg300=Z.ZZ total=...
    // full avg10=X.XX avg60=Y.YY avg300=Z.ZZ total=...
    
    std::string line;
    bool found_some = false;
    bool found_full = false;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string type;
        iss >> type;
        
        if (type == "some") {
            std::string avg10_str;
            iss >> avg10_str; // e.g., "avg10=0.00"
            if (avg10_str.rfind("avg10=", 0) == 0) {
                try {
                    sample.cpu_some_avg10 = std::stod(avg10_str.substr(6));
                    found_some = true;
                } catch (...) {
                    return false;
                }
            }
        } else if (type == "full") {
            std::string avg10_str;
            iss >> avg10_str; // e.g., "avg10=0.00"
            if (avg10_str.rfind("avg10=", 0) == 0) {
                try {
                    sample.cpu_full_avg10 = std::stod(avg10_str.substr(6));
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
