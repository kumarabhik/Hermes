#include "hermes/actions/reprioritize.hpp"

#include <algorithm>
#include <sstream>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/time.h>
#include <cerrno>
#include <cstring>
#endif

namespace hermes {

ReprioritizeAction::ReprioritizeAction(ReprioritizeConfig config)
    : config_(config) {}

InterventionResult ReprioritizeAction::execute(const InterventionDecision& decision) {
    InterventionResult result;
    result.ts_mono = decision.ts_mono;
    last_records_.clear();

    if (decision.target_pids.empty()) {
        result.success = true;
        result.system_effect = "no-op: no target pids";
        result.reversal_condition = "none";
        return result;
    }

    std::ostringstream effect;
    bool any_failure = false;

    for (const int pid : decision.target_pids) {
        ReprioritizeRecord record;
        record.pid = pid;

#ifdef __linux__
        errno = 0;
        const int current_nice = getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
        if (errno != 0) {
            record.error = std::string("getpriority failed: ") + std::strerror(errno);
            record.applied = false;
            any_failure = true;
            last_records_.push_back(record);
            effect << "[pid=" << pid << " err=" << record.error << "]";
            continue;
        }
        record.previous_nice = current_nice;

        const int target = std::min(
            current_nice + config_.max_nice_increase,
            config_.target_nice);
        record.applied_nice = target;

        if (setpriority(PRIO_PROCESS, static_cast<id_t>(pid), target) != 0) {
            record.error = std::string("setpriority failed: ") + std::strerror(errno);
            record.applied = false;
            any_failure = true;
            last_records_.push_back(record);
            effect << "[pid=" << pid << " err=" << record.error << "]";
            continue;
        }
        record.applied = true;
        effect << "[pid=" << pid << " nice=" << current_nice << "->" << target << "]";
#else
        record.previous_nice = 0;
        record.applied_nice = config_.target_nice;
        record.applied = false;
        record.error = "setpriority not available on this platform";
        effect << "[pid=" << pid << " simulated: nice->" << config_.target_nice << "]";
#endif
        last_records_.push_back(record);
    }

    result.success = !any_failure;
    result.system_effect = "reprioritize: " + effect.str();
    result.reversal_condition =
        "UPS returns to normal band and risk drops to low for 3 consecutive cycles; "
        "call restore() with last_records() to revert nice values";
    return result;
}

void ReprioritizeAction::restore(const std::vector<ReprioritizeRecord>& records) {
#ifdef __linux__
    for (const ReprioritizeRecord& record : records) {
        if (!record.applied) {
            continue;
        }
        setpriority(PRIO_PROCESS, static_cast<id_t>(record.pid), record.previous_nice);
    }
#else
    (void)records;
#endif
}

} // namespace hermes
