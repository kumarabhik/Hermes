#include "hermes/profiler/workload_classifier.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace hermes {
namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_any(const std::string& haystack, const std::initializer_list<const char*>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return haystack.find(needle) != std::string::npos;
    });
}

} // namespace

void WorkloadClassifier::classify(std::vector<ProcessSnapshot>& processes) const {
    for (ProcessSnapshot& process : processes) {
        const std::string lower_cmd = to_lower_copy(process.cmd);

        process.foreground = false;
        process.protected_process = contains_any(lower_cmd, {"hermesd", "hermesctl"});

        if (process.protected_process) {
            process.workload_class = WorkloadClass::Background;
            continue;
        }

        if (process.gpu_mb < 1.0 && process.cpu_pct < 1.0) {
            process.workload_class = WorkloadClass::Idle;
            continue;
        }

        if (contains_any(lower_cmd, {"train", "trainer", "torchrun", "deepspeed"}) ||
            (contains_any(lower_cmd, {"python", "python3"}) && process.gpu_mb > 256.0)) {
            process.workload_class = WorkloadClass::Training;
            continue;
        }

        if (contains_any(lower_cmd, {"infer", "inference", "serve", "server", "api"})) {
            process.workload_class = WorkloadClass::Inference;
            process.foreground = true;
            continue;
        }

        process.workload_class = WorkloadClass::Background;
    }
}

} // namespace hermes
