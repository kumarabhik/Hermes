#pragma once

#include "hermes/core/types.hpp"

#include <string>
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
    InterventionResult execute(const InterventionDecision& decision);

    const std::vector<KillRecord>& last_records() const {
        return last_records_;
    }

private:
    bool is_guardrailed(int pid, const std::string& cmd, std::string& reason) const;

    KillConfig config_;
    std::vector<KillRecord> last_records_;
    int self_pid_{-1};
};

} // namespace hermes
