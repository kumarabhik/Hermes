#include "hermes/engine/predictor.hpp"

#include <algorithm>
#include <cmath>

namespace hermes {
namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool is_gpu_candidate(const ProcessSnapshot& process) {
    return !process.protected_process && !process.foreground && process.gpu_mb > 0.0;
}

} // namespace

void OomPredictor::prune_history(uint64_t now_mono) {
    while (!history_.empty() && history_.front().ts_mono + max_window_ms_ < now_mono) {
        history_.pop_front();
    }
}

double OomPredictor::compute_vram_slope_mb_s() const {
    if (history_.size() < 2) {
        return 0.0;
    }

    const PressureSample& first = history_.front();
    const PressureSample& last = history_.back();
    if (last.ts_mono <= first.ts_mono) {
        return 0.0;
    }

    const double delta_mb = last.vram_used_mb - first.vram_used_mb;
    const double delta_s = static_cast<double>(last.ts_mono - first.ts_mono) / 1000.0;
    return delta_s > 0.0 ? delta_mb / delta_s : 0.0;
}

std::vector<int> OomPredictor::select_target_pids(const std::vector<ProcessSnapshot>& processes) const {
    std::vector<const ProcessSnapshot*> candidates;
    for (const ProcessSnapshot& process : processes) {
        if (is_gpu_candidate(process)) {
            candidates.push_back(&process);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const ProcessSnapshot* left, const ProcessSnapshot* right) {
        if (left->gpu_mb != right->gpu_mb) {
            return left->gpu_mb > right->gpu_mb;
        }
        return left->cpu_pct > right->cpu_pct;
    });

    std::vector<int> pids;
    const std::size_t limit = std::min<std::size_t>(2, candidates.size());
    for (std::size_t index = 0; index < limit; ++index) {
        pids.push_back(candidates[index]->pid);
    }
    return pids;
}

RiskPrediction OomPredictor::update(
    const PressureSample& sample,
    const std::vector<ProcessSnapshot>& processes,
    const PressureScore& score) {
    history_.push_back(sample);
    prune_history(sample.ts_mono);

    RiskPrediction prediction;
    prediction.ts_mono = sample.ts_mono;
    prediction.target_pids = select_target_pids(processes);

    const double vram_headroom_pct =
        sample.vram_total_mb > 0.0 ? clamp01(sample.vram_free_mb / sample.vram_total_mb) * 100.0 : 100.0;
    const double vram_slope_mb_s = compute_vram_slope_mb_s();

    double risk_score = clamp01(score.ups / 100.0) * 0.40;

    if (vram_headroom_pct <= 10.0) {
        risk_score += 0.20;
        prediction.reason_codes.push_back("VRAM_HEADROOM_LOW");
    }
    if (vram_headroom_pct <= 5.0) {
        risk_score += 0.20;
        prediction.reason_codes.push_back("VRAM_HEADROOM_COLLAPSE");
    }
    if (vram_slope_mb_s >= 50.0) {
        risk_score += 0.15;
        prediction.reason_codes.push_back("VRAM_GROWTH_FAST");
    }
    if (vram_slope_mb_s >= 200.0) {
        risk_score += 0.15;
        prediction.reason_codes.push_back("VRAM_GROWTH_CRITICAL");
    }
    if (sample.mem_full_avg10 >= 2.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("MEM_FULL_PSI_RISING");
    }
    if (sample.mem_full_avg10 >= 5.0) {
        risk_score += 0.15;
        prediction.reason_codes.push_back("MEM_FULL_PSI_CRITICAL");
    }
    if (prediction.target_pids.size() > 1 && sample.gpu_util_pct >= 80.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("MULTI_PROC_GPU_CONTENTION");
    }

    prediction.risk_score = clamp01(risk_score);

    if (vram_slope_mb_s > 0.0) {
        prediction.lead_time_s = std::max(0.0, sample.vram_free_mb / vram_slope_mb_s);
    }

    const bool vram_driven = vram_headroom_pct <= 10.0 || vram_slope_mb_s >= 50.0;
    const bool mixed_pressure = sample.mem_full_avg10 >= 2.0 && score.band == PressureBand::Critical;

    if (mixed_pressure) {
        prediction.predicted_event = "mixed_pressure_collapse";
    } else if (vram_driven) {
        prediction.predicted_event = "gpu_oom";
    } else if (sample.mem_full_avg10 >= 1.0 || sample.cpu_full_avg10 >= 1.0) {
        prediction.predicted_event = "latency_spike";
    }

    if (prediction.risk_score >= 0.85) {
        prediction.risk_band = RiskBand::Critical;
        prediction.recommended_action = ActionKind::TerminateCandidate;
    } else if (prediction.risk_score >= 0.70) {
        prediction.risk_band = RiskBand::High;
        prediction.recommended_action = ActionKind::Throttle;
    } else if (prediction.risk_score >= 0.40) {
        prediction.risk_band = RiskBand::Medium;
        prediction.recommended_action = ActionKind::Reprioritize;
    } else {
        prediction.risk_band = RiskBand::Low;
        prediction.recommended_action = ActionKind::Observe;
    }

    if (prediction.reason_codes.empty()) {
        prediction.reason_codes.push_back("PRESSURE_STABLE");
    }

    return prediction;
}

} // namespace hermes
