#pragma once

#include "hermes/core/types.hpp"

#include <chrono>
#include <unordered_set>
#include <vector>

namespace hermes {

// Level 2 action: pause background processes using SIGSTOP and resume with SIGCONT.
// On Linux, uses kill(pid, SIGSTOP) and kill(pid, SIGCONT).
// On other platforms, logs a simulated effect without mutating the host.
//
// Safety: ThrottleAction tracks paused PIDs and refuses to double-pause.
// Reversal: call resume() when UPS drops below elevated band for the hysteresis window.

struct ThrottleRecord {
    int pid{-1};
    bool paused{false};
    bool resumed{false};
    std::string error;
};

class ThrottleAction {
public:
    ThrottleAction() = default;

    // Pause target PIDs with SIGSTOP. Skips PIDs already paused.
    InterventionResult pause(const InterventionDecision& decision);

    // Resume target PIDs with SIGCONT. Clears the paused PID set.
    InterventionResult resume(const InterventionDecision& decision);

    // Check whether a pid is currently paused by this executor.
    bool is_paused(int pid) const;

    // Resume all currently paused PIDs (called during Recovery state entry).
    InterventionResult resume_all(uint64_t ts_mono);

    const std::vector<ThrottleRecord>& last_records() const {
        return last_records_;
    }

private:
    std::unordered_set<int> paused_pids_;
    std::vector<ThrottleRecord> last_records_;
};

} // namespace hermes
