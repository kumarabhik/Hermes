#pragma once

#include "hermes/core/types.hpp"

#include <deque>
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
    double compute_vram_slope_mb_s() const;
    std::vector<int> select_target_pids(const std::vector<ProcessSnapshot>& processes) const;

    std::deque<PressureSample> history_;
    uint64_t max_window_ms_{10000};
};

} // namespace hermes
