#pragma once

#include "hermes/core/types.hpp"

#include <vector>

namespace hermes {

// Level 1 action: raise the nice value (lower scheduling priority) of target processes.
// On Linux, uses setpriority(PRIO_PROCESS, pid, nice_value).
// On other platforms, logs a simulated effect without mutating the host.
//
// Reversal: the caller is responsible for restoring nice values when pressure subsides.
// Each ReprioritizeAction call records the original nice value in the result for rollback.

struct ReprioritizeConfig {
    int target_nice{10};      // nice value to apply (0-19; higher = lower priority)
    int max_nice_increase{10}; // cap to avoid excessively low priority
};

struct ReprioritizeRecord {
    int pid{-1};
    int previous_nice{0};
    int applied_nice{0};
    bool applied{false};
    std::string error;
};

class ReprioritizeAction {
public:
    explicit ReprioritizeAction(ReprioritizeConfig config = {});

    // Execute the reprioritize action. Returns a result with system_effect and
    // reversal_condition populated. On Linux, actually calls setpriority().
    InterventionResult execute(const InterventionDecision& decision);

    // Restore original nice values for all PIDs tracked in records.
    // Intended to be called when UPS returns to normal.
    void restore(const std::vector<ReprioritizeRecord>& records);

    const std::vector<ReprioritizeRecord>& last_records() const {
        return last_records_;
    }

private:
    ReprioritizeConfig config_;
    std::vector<ReprioritizeRecord> last_records_;
};

} // namespace hermes
