#pragma once

#include "hermes/core/types.hpp"

#include <deque>
#include <unordered_map>
#include <vector>

namespace hermes {

class OomPredictor {
public:
    RiskPrediction update(
        const PressureSample& sample,
        const std::vector<ProcessSnapshot>& processes,
        const PressureScore& score);

private:
    void prune_history(uint64_t now_mono);

    // Dual window VRAM slopes per the design spec (3s fast + 10s medium)
    double compute_vram_slope_mb_s(uint64_t window_ms) const;

    // Per-PID GPU memory growth tracking
    double compute_pid_gpu_growth_mb_s(int pid) const;
    void update_pid_gpu_history(const std::vector<ProcessSnapshot>& processes, uint64_t ts_mono);

    std::vector<int> select_target_pids(const std::vector<ProcessSnapshot>& processes) const;

    // Rolling sample history (10s max window)
    std::deque<PressureSample> history_;
    uint64_t max_window_ms_{10000};

    // Per-PID GPU memory history for growth tracking
    struct PidGpuSample {
        uint64_t ts_mono{0};
        double gpu_mb{0.0};
    };
    std::unordered_map<int, std::deque<PidGpuSample>> pid_gpu_history_;
    uint64_t pid_gpu_window_ms_{10000};

    // Sustained pressure residency counters
    int critical_band_cycles_{0};      // consecutive cycles in critical UPS band
    int high_risk_cycles_{0};          // consecutive cycles with high/critical risk
    int mem_psi_elevated_cycles_{0};   // consecutive cycles with mem_full_avg10 >= 2
    int io_psi_elevated_cycles_{0};    // consecutive cycles with io_full_avg10 >= 1
};

} // namespace hermes
