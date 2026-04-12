#pragma once

#include "hermes/core/types.hpp"

namespace hermes {

struct PressureScoreConfig {
    double cpu_weight{0.20};
    double mem_weight{0.35};
    double gpu_weight{0.15};
    double vram_weight{0.30};
    double elevated_threshold{40.0};
    double critical_threshold{70.0};
};

class PressureScoreCalculator {
public:
    explicit PressureScoreCalculator(PressureScoreConfig config = {});

    PressureScore compute(const PressureSample& sample) const;

private:
    PressureScoreConfig config_;
};

} // namespace hermes
