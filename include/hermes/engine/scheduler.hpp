#pragma once

#include "hermes/core/types.hpp"

#include <unordered_map>
#include <vector>

namespace hermes {

struct SchedulerConfig {
    OperatingMode mode{OperatingMode::ObserveOnly};
    uint64_t level1_cooldown_ms{15000};
    uint64_t level2_cooldown_ms{20000};
    uint64_t level3_cooldown_ms{300000};
    int stable_cycles_for_recovery{3};
};

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig config = {});

    InterventionDecision evaluate(
        const PressureScore& score,
        const RiskPrediction& risk,
        const std::vector<ProcessSnapshot>& processes);

    SchedulerState state() const {
        return state_;
    }

private:
    const ProcessSnapshot* choose_candidate(const std::vector<ProcessSnapshot>& processes) const;
    bool pid_in_cooldown(int pid, uint64_t now) const;
    void set_pid_cooldown(int pid, uint64_t now, uint64_t cooldown_ms);

    SchedulerConfig config_;
    SchedulerState state_{SchedulerState::Normal};
    std::unordered_map<int, uint64_t> pid_cooldowns_;
    uint64_t global_level3_cooldown_until_{0};
    int stable_cycle_count_{0};
};

} // namespace hermes
