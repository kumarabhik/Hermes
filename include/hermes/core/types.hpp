#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hermes {

enum class WorkloadClass {
    Unknown,
    Training,
    Inference,
    Background,
    Idle
};

enum class PressureBand {
    Normal,
    Elevated,
    Critical
};

enum class RiskBand {
    Low,
    Medium,
    High,
    Critical
};

enum class SchedulerState {
    Normal,
    Elevated,
    Throttled,
    Recovery,
    Cooldown
};

enum class ActionLevel {
    None,
    Level1,
    Level2,
    Level3
};

enum class ActionKind {
    Observe,
    Reprioritize,
    Throttle,
    Resume,
    TerminateCandidate
};

enum class OperatingMode {
    ObserveOnly,
    Advisory,
    ActiveControl
};

inline const char* to_string(WorkloadClass workload_class) {
    switch (workload_class) {
    case WorkloadClass::Training:
        return "training";
    case WorkloadClass::Inference:
        return "inference";
    case WorkloadClass::Background:
        return "background";
    case WorkloadClass::Idle:
        return "idle";
    case WorkloadClass::Unknown:
    default:
        return "unknown";
    }
}

inline const char* to_string(PressureBand band) {
    switch (band) {
    case PressureBand::Elevated:
        return "elevated";
    case PressureBand::Critical:
        return "critical";
    case PressureBand::Normal:
    default:
        return "normal";
    }
}

inline const char* to_string(RiskBand band) {
    switch (band) {
    case RiskBand::Medium:
        return "medium";
    case RiskBand::High:
        return "high";
    case RiskBand::Critical:
        return "critical";
    case RiskBand::Low:
    default:
        return "low";
    }
}

inline const char* to_string(SchedulerState state) {
    switch (state) {
    case SchedulerState::Elevated:
        return "elevated";
    case SchedulerState::Throttled:
        return "throttled";
    case SchedulerState::Recovery:
        return "recovery";
    case SchedulerState::Cooldown:
        return "cooldown";
    case SchedulerState::Normal:
    default:
        return "normal";
    }
}

inline const char* to_string(ActionLevel level) {
    switch (level) {
    case ActionLevel::Level1:
        return "level1";
    case ActionLevel::Level2:
        return "level2";
    case ActionLevel::Level3:
        return "level3";
    case ActionLevel::None:
    default:
        return "none";
    }
}

inline const char* to_string(ActionKind action) {
    switch (action) {
    case ActionKind::Reprioritize:
        return "reprioritize";
    case ActionKind::Throttle:
        return "throttle";
    case ActionKind::Resume:
        return "resume";
    case ActionKind::TerminateCandidate:
        return "terminate_candidate";
    case ActionKind::Observe:
    default:
        return "observe";
    }
}

inline const char* to_string(OperatingMode mode) {
    switch (mode) {
    case OperatingMode::Advisory:
        return "advisory";
    case OperatingMode::ActiveControl:
        return "active-control";
    case OperatingMode::ObserveOnly:
    default:
        return "observe-only";
    }
}

struct PressureSample {
    uint64_t ts_wall{0};
    uint64_t ts_mono{0};
    double cpu_some_avg10{0.0};
    double cpu_full_avg10{0.0};
    double mem_some_avg10{0.0};
    double mem_full_avg10{0.0};
    double gpu_util_pct{0.0};
    double vram_used_mb{0.0};
    double vram_total_mb{0.0};
    double vram_free_mb{0.0};
    uint32_t loadavg_runnable{0};
};

inline void stamp_pressure_sample(PressureSample& sample) {
    sample.ts_wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sample.ts_mono = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct GpuProcessUsage {
    int pid{-1};
    double gpu_mb{0.0};
};

struct ProcessSnapshot {
    int pid{-1};
    int ppid{-1};
    std::string cmd;
    std::string state;
    int nice{0};
    double cpu_pct{0.0};
    double rss_mb{0.0};
    double gpu_mb{0.0};
    WorkloadClass workload_class{WorkloadClass::Unknown};
    bool foreground{false};
    bool protected_process{false};
    uint64_t total_cpu_ticks{0};
};

struct PressureComponents {
    double n_cpu{0.0};
    double n_mem{0.0};
    double n_gpu_util{0.0};
    double n_vram{0.0};
    double weighted_cpu{0.0};
    double weighted_mem{0.0};
    double weighted_gpu_util{0.0};
    double weighted_vram{0.0};
};

struct PressureScore {
    uint64_t ts_mono{0};
    double ups{0.0};
    PressureBand band{PressureBand::Normal};
    PressureComponents components{};
    std::vector<std::string> dominant_signals;
};

struct RiskPrediction {
    uint64_t ts_mono{0};
    double risk_score{0.0};
    RiskBand risk_band{RiskBand::Low};
    std::string predicted_event{"none"};
    double lead_time_s{0.0};
    std::vector<std::string> reason_codes;
    std::vector<int> target_pids;
    ActionKind recommended_action{ActionKind::Observe};
};

struct InterventionDecision {
    uint64_t ts_mono{0};
    ActionLevel level{ActionLevel::None};
    ActionKind action{ActionKind::Observe};
    std::vector<int> target_pids;
    SchedulerState previous_scheduler_state{SchedulerState::Normal};
    SchedulerState scheduler_state{SchedulerState::Normal};
    bool scheduler_state_changed{false};
    std::string cooldown_state{"clear"};
    std::string why;
    OperatingMode mode{OperatingMode::ObserveOnly};
    bool should_execute{false};
};

struct InterventionResult {
    uint64_t ts_mono{0};
    bool success{false};
    std::string error;
    std::string system_effect;
    bool reverted{false};
};

} // namespace hermes
