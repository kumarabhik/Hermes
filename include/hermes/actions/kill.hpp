#pragma once

#include "hermes/core/types.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes {

// Level 3 action: terminate a candidate process with SIGTERM (then SIGKILL on Linux).
// Subject to strict guardrails: protected PIDs, Hermes own PID, and name-pattern exclusions
// can never be targeted regardless of mode.
//
// This is IRREVERSIBLE. Only call when active-control mode is explicitly enabled and
// the operator has acknowledged that termination is authorized.

struct KillConfig {
    bool use_sigkill{false};           // true = SIGKILL, false = SIGTERM first
    std::unordered_set<int> protected_pids;   // absolute PID protection list
    std::vector<std::string> protected_name_patterns; // name substrings that are always safe

    // Placement-aware kill routing (multi-GPU).
    // When placement_aware_kills=true, target PIDs are re-ordered to prefer processes
    // on the GPU device with the highest current utilization.  This is populated by
    // the daemon from NvmlBackend data before each kill decision.
    bool placement_aware_kills{false};
    std::unordered_map<int, int>    pid_device;   // pid → primary GPU device index
    std::unordered_map<int, double> device_util;  // device index → current GPU util%
};

struct KillRecord {
    int pid{-1};
    bool terminated{false};
    std::string signal_sent;
    std::string error;
    std::string reject_reason; // populated when guardrail blocked the kill
};

class KillAction {
public:
    explicit KillAction(KillConfig config = {});

    // Attempt to terminate the first eligible pid in decision.target_pids.
    // Enforces all guardrails before calling kill(). Returns structured result.
    // On non-Linux platforms, records a simulated effect without mutating host.
    //
    // When config_.placement_aware_kills is true, target_pids are re-ordered so
    // processes on the GPU device with the highest current utilization are
    // preferred.  This gives the scheduler a chance to free pressure on the
    // hottest device first.
    InterventionResult execute(const InterventionDecision& decision);

    // Update live GPU placement data for the next execute() call.
    // Called by the daemon before each decision cycle when NVML is available.
    void update_placement_data(
        const std::unordered_map<int, int>&    pid_device,
        const std::unordered_map<int, double>& device_util);

    const std::vector<KillRecord>& last_records() const {
        return last_records_;
    }

private:
    bool is_guardrailed(int pid, const std::string& cmd, std::string& reason) const;

    // Re-order pids so those on the highest-utilization GPU device come first.
    // Pids with no device attribution are placed last.
    std::vector<int> sort_by_placement(const std::vector<int>& pids) const;

    KillConfig config_;
    std::vector<KillRecord> last_records_;
    int self_pid_{-1};
};

} // namespace hermes
