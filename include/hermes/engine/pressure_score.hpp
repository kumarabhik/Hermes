#pragma once

#include "hermes/core/types.hpp"

namespace hermes {

struct PressureScoreConfig {
    double cpu_weight{0.20};
    double mem_weight{0.35};
    double gpu_weight{0.15};
    double vram_weight{0.30};
    // IO PSI weight. Default 0.0 (no effect) so the existing formula is unchanged
    // on Tier A environments where IO PSI is unavailable. Set to e.g. 0.08 on
    // Tier B/C hosts and reduce other weights proportionally.
    double io_weight{0.0};
    double elevated_threshold{40.0};
    double critical_threshold{70.0};
};

class PressureScoreCalculator {
public:
    explicit PressureScoreCalculator(PressureScoreConfig config = {});

    PressureScore compute(const PressureSample& sample);

private:
    PressureScoreConfig config_;
    PressureBand last_band_{PressureBand::Normal};
    bool first_sample_{true};
};

} // namespace hermes
