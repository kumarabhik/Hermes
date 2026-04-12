#include "hermes/engine/scheduler.hpp"

#include <algorithm>
#include <sstream>

namespace hermes {
namespace {

bool is_stable(const PressureScore& score, const RiskPrediction& risk) {
    return score.band == PressureBand::Normal && risk.risk_band == RiskBand::Low;
}

std::string join_reasons(const std::vector<std::string>& reasons) {
    std::ostringstream oss;
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << reasons[index];
    }
    return oss.str();
}

} // namespace

Scheduler::Scheduler(SchedulerConfig config)
    : config_(config) {}

const ProcessSnapshot* Scheduler::choose_candidate(const std::vector<ProcessSnapshot>& processes) const {
    const ProcessSnapshot* selected = nullptr;

    for (const ProcessSnapshot& process : processes) {
        if (process.protected_process || process.foreground) {
            continue;
        }

        if (selected == nullptr) {
            selected = &process;
            continue;
        }

        const bool preferred_class =
            (process.workload_class == WorkloadClass::Background && selected->workload_class != WorkloadClass::Background) ||
            (process.workload_class == WorkloadClass::Training && selected->workload_class == WorkloadClass::Inference);

        if (preferred_class ||
            process.gpu_mb > selected->gpu_mb ||
            (process.gpu_mb == selected->gpu_mb && process.cpu_pct > selected->cpu_pct)) {
            selected = &process;
        }
    }

    return selected;
}

bool Scheduler::pid_in_cooldown(int pid, uint64_t now) const {
    const auto it = pid_cooldowns_.find(pid);
    return it != pid_cooldowns_.end() && it->second > now;
}

void Scheduler::set_pid_cooldown(int pid, uint64_t now, uint64_t cooldown_ms) {
    pid_cooldowns_[pid] = now + cooldown_ms;
}

InterventionDecision Scheduler::evaluate(
    const PressureScore& score,
    const RiskPrediction& risk,
    const std::vector<ProcessSnapshot>& processes) {
    InterventionDecision decision;
    decision.ts_mono = score.ts_mono;
    decision.mode = config_.mode;
    const SchedulerState entry_state = state_;

    auto finalize = [&](InterventionDecision& candidate_decision) {
        candidate_decision.previous_scheduler_state = entry_state;
        candidate_decision.scheduler_state = state_;
        candidate_decision.scheduler_state_changed = entry_state != state_;
        return candidate_decision;
    };

    if (is_stable(score, risk)) {
        ++stable_cycle_count_;
    } else {
        stable_cycle_count_ = 0;
    }

    if (state_ == SchedulerState::Cooldown && stable_cycle_count_ >= config_.stable_cycles_for_recovery) {
        state_ = SchedulerState::Recovery;
    } else if ((state_ == SchedulerState::Recovery || state_ == SchedulerState::Elevated) &&
               stable_cycle_count_ >= config_.stable_cycles_for_recovery) {
        state_ = SchedulerState::Normal;
    } else if (!is_stable(score, risk) && state_ == SchedulerState::Recovery) {
        state_ = SchedulerState::Elevated;
    }

    const ProcessSnapshot* candidate = choose_candidate(processes);
    const uint64_t now = score.ts_mono;

    if (global_level3_cooldown_until_ > now) {
        state_ = SchedulerState::Cooldown;
        decision.cooldown_state = "global-level3";
        decision.why = "Level 3 cooldown is active";
        return finalize(decision);
    }

    if (risk.risk_band == RiskBand::Critical &&
        score.band == PressureBand::Critical &&
        candidate != nullptr) {
        state_ = SchedulerState::Cooldown;
        decision.level = ActionLevel::Level3;
        decision.action = ActionKind::TerminateCandidate;
        decision.target_pids = {candidate->pid};
        decision.cooldown_state = "global-level3";
        decision.why = "Critical risk with critical UPS; reasons=" + join_reasons(risk.reason_codes);
        decision.should_execute = config_.mode != OperatingMode::ObserveOnly;
        global_level3_cooldown_until_ = now + config_.level3_cooldown_ms;
        return finalize(decision);
    }

    if ((risk.risk_band == RiskBand::High || score.band == PressureBand::Critical) &&
        candidate != nullptr) {
        if (pid_in_cooldown(candidate->pid, now)) {
            state_ = SchedulerState::Cooldown;
            decision.cooldown_state = "pid-level2";
            decision.why = "Level 2 cooldown active for target pid=" + std::to_string(candidate->pid);
            return finalize(decision);
        }

        state_ = SchedulerState::Throttled;
        decision.level = ActionLevel::Level2;
        decision.action = ActionKind::Throttle;
        decision.target_pids = {candidate->pid};
        decision.why = "High risk or critical UPS; reasons=" + join_reasons(risk.reason_codes);
        decision.should_execute = config_.mode != OperatingMode::ObserveOnly;
        set_pid_cooldown(candidate->pid, now, config_.level2_cooldown_ms);
        return finalize(decision);
    }

    if ((risk.risk_band == RiskBand::Medium || score.band == PressureBand::Elevated) &&
        candidate != nullptr) {
        if (pid_in_cooldown(candidate->pid, now)) {
            state_ = SchedulerState::Elevated;
            decision.cooldown_state = "pid-level1";
            decision.why = "Level 1 cooldown active for target pid=" + std::to_string(candidate->pid);
            return finalize(decision);
        }

        state_ = SchedulerState::Elevated;
        decision.level = ActionLevel::Level1;
        decision.action = ActionKind::Reprioritize;
        decision.target_pids = {candidate->pid};
        decision.why = "Elevated contention; reasons=" + join_reasons(risk.reason_codes);
        decision.should_execute = config_.mode != OperatingMode::ObserveOnly;
        set_pid_cooldown(candidate->pid, now, config_.level1_cooldown_ms);
        return finalize(decision);
    }

    decision.why = stable_cycle_count_ >= config_.stable_cycles_for_recovery
        ? "Pressure stable"
        : "No eligible action";
    return finalize(decision);
}

} // namespace hermes
