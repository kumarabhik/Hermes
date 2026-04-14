#include "hermes/engine/pressure_score.hpp"

#include <algorithm>
#include <utility>

namespace hermes {
namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

PressureScoreCalculator::PressureScoreCalculator(PressureScoreConfig config)
    : config_(config) {}

PressureScore PressureScoreCalculator::compute(const PressureSample& sample) {
    PressureScore score;
    score.ts_mono = sample.ts_mono;

    score.components.n_cpu = clamp01(sample.cpu_some_avg10 / 25.0);
    score.components.n_mem = std::max(
        clamp01(sample.mem_full_avg10 / 5.0),
        0.5 * clamp01(sample.mem_some_avg10 / 20.0));
    score.components.n_gpu_util = clamp01(sample.gpu_util_pct / 100.0);
    score.components.n_vram =
        sample.vram_total_mb > 0.0 ? clamp01(sample.vram_used_mb / sample.vram_total_mb) : 0.0;
    // IO PSI: weighted more toward full stalls (like mem). Only active when io_weight > 0.
    score.components.n_io = std::max(
        clamp01(sample.io_full_avg10 / 5.0),
        0.5 * clamp01(sample.io_some_avg10 / 20.0));

    score.components.weighted_cpu = config_.cpu_weight * score.components.n_cpu;
    score.components.weighted_mem = config_.mem_weight * score.components.n_mem;
    score.components.weighted_gpu_util = config_.gpu_weight * score.components.n_gpu_util;
    score.components.weighted_vram = config_.vram_weight * score.components.n_vram;
    score.components.weighted_io = config_.io_weight * score.components.n_io;

    score.ups = 100.0 * (
        score.components.weighted_cpu +
        score.components.weighted_mem +
        score.components.weighted_gpu_util +
        score.components.weighted_vram +
        score.components.weighted_io);

    if (score.ups >= config_.critical_threshold) {
        score.band = PressureBand::Critical;
    } else if (score.ups >= config_.elevated_threshold) {
        score.band = PressureBand::Elevated;
    } else {
        score.band = PressureBand::Normal;
    }

    std::vector<std::pair<std::string, double>> weighted_signals = {
        {"mem", score.components.weighted_mem},
        {"vram", score.components.weighted_vram},
        {"cpu", score.components.weighted_cpu},
        {"gpu", score.components.weighted_gpu_util},
        {"io",  score.components.weighted_io},
    };
    std::sort(weighted_signals.begin(), weighted_signals.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });

    for (const auto& signal : weighted_signals) {
        if (signal.second > 0.0) {
            score.dominant_signals.push_back(signal.first);
        }
    }

    score.previous_band = last_band_;
    score.band_changed = !first_sample_ && (score.band != last_band_);
    last_band_ = score.band;
    first_sample_ = false;

    return score;
}

} // namespace hermes
