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

// Compute VRAM growth slope over a given window (ms).
// Uses the earliest sample within that window vs. the latest sample.
double OomPredictor::compute_vram_slope_mb_s(uint64_t window_ms) const {
    if (history_.size() < 2) {
        return 0.0;
    }

    const PressureSample& latest = history_.back();
    const uint64_t cutoff = latest.ts_mono > window_ms ? latest.ts_mono - window_ms : 0;

    const PressureSample* earliest_in_window = nullptr;
    for (const auto& s : history_) {
        if (s.ts_mono >= cutoff) {
            earliest_in_window = &s;
            break;
        }
    }

    if (earliest_in_window == nullptr || latest.ts_mono <= earliest_in_window->ts_mono) {
        return 0.0;
    }

    const double delta_mb = latest.vram_used_mb - earliest_in_window->vram_used_mb;
    const double delta_s = static_cast<double>(latest.ts_mono - earliest_in_window->ts_mono) / 1000.0;
    return delta_s > 0.0 ? delta_mb / delta_s : 0.0;
}

void OomPredictor::update_pid_gpu_history(
    const std::vector<ProcessSnapshot>& processes,
    uint64_t ts_mono) {

    // Prune old entries and add new ones
    for (const ProcessSnapshot& proc : processes) {
        if (proc.pid <= 0) continue;
        auto& q = pid_gpu_history_[proc.pid];
        // Prune entries older than the window
        while (!q.empty() && q.front().ts_mono + pid_gpu_window_ms_ < ts_mono) {
            q.pop_front();
        }
        q.push_back({ts_mono, proc.gpu_mb});
    }

    // Remove PIDs no longer present
    std::vector<int> active_pids;
    for (const auto& p : processes) {
        active_pids.push_back(p.pid);
    }
    for (auto it = pid_gpu_history_.begin(); it != pid_gpu_history_.end();) {
        if (std::find(active_pids.begin(), active_pids.end(), it->first) == active_pids.end()) {
            it = pid_gpu_history_.erase(it);
        } else {
            ++it;
        }
    }
}

double OomPredictor::compute_pid_gpu_growth_mb_s(int pid) const {
    const auto it = pid_gpu_history_.find(pid);
    if (it == pid_gpu_history_.end() || it->second.size() < 2) {
        return 0.0;
    }
    const auto& q = it->second;
    const PidGpuSample& first = q.front();
    const PidGpuSample& last = q.back();
    if (last.ts_mono <= first.ts_mono) return 0.0;
    const double delta_mb = last.gpu_mb - first.gpu_mb;
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

    std::sort(candidates.begin(), candidates.end(), [this](const ProcessSnapshot* left, const ProcessSnapshot* right) {
        // Prefer processes that are growing their GPU footprint fastest
        const double left_growth = compute_pid_gpu_growth_mb_s(left->pid);
        const double right_growth = compute_pid_gpu_growth_mb_s(right->pid);
        if (std::abs(left_growth - right_growth) > 1.0) {
            return left_growth > right_growth;
        }
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
    update_pid_gpu_history(processes, sample.ts_mono);

    // Update sustained residency counters
    if (score.band == PressureBand::Critical) {
        ++critical_band_cycles_;
    } else {
        critical_band_cycles_ = 0;
    }

    if (score.band == PressureBand::Elevated || score.band == PressureBand::Critical) {
        // only tracks high/critical risk once we have a prior prediction
    }

    if (sample.mem_full_avg10 >= 2.0) {
        ++mem_psi_elevated_cycles_;
    } else {
        mem_psi_elevated_cycles_ = 0;
    }

    if (sample.io_full_avg10 >= 1.0) {
        ++io_psi_elevated_cycles_;
    } else {
        io_psi_elevated_cycles_ = 0;
    }

    RiskPrediction prediction;
    prediction.ts_mono = sample.ts_mono;
    prediction.target_pids = select_target_pids(processes);

    const double vram_headroom_pct =
        sample.vram_total_mb > 0.0
            ? clamp01(sample.vram_free_mb / sample.vram_total_mb) * 100.0
            : 100.0;

    // Dual-window VRAM slopes: fast (3s) catches bursts; medium (10s) catches trends
    const double vram_slope_fast_mb_s = compute_vram_slope_mb_s(3000);
    const double vram_slope_med_mb_s  = compute_vram_slope_mb_s(10000);
    const double vram_slope_mb_s = std::max(vram_slope_fast_mb_s, vram_slope_med_mb_s);

    // Base risk from UPS band (max 40% contribution)
    double risk_score = clamp01(score.ups / 100.0) * 0.40;

    // VRAM headroom signals
    if (vram_headroom_pct <= 15.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("VRAM_HEADROOM_LOW");
    }
    if (vram_headroom_pct <= 10.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("VRAM_HEADROOM_CRITICAL");
    }
    if (vram_headroom_pct <= 5.0) {
        risk_score += 0.15;
        prediction.reason_codes.push_back("VRAM_HEADROOM_COLLAPSE");
    }

    // Fast VRAM slope (burst allocation detection)
    if (vram_slope_fast_mb_s >= 100.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("VRAM_BURST_ALLOC");
    }

    // Medium VRAM slope (sustained growth)
    if (vram_slope_med_mb_s >= 50.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("VRAM_GROWTH_FAST");
    }
    if (vram_slope_med_mb_s >= 200.0) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("VRAM_GROWTH_CRITICAL");
    }

    // Per-PID GPU growth tracking: flag top-growing process
    for (const int pid : prediction.target_pids) {
        const double growth = compute_pid_gpu_growth_mb_s(pid);
        if (growth >= 30.0) {
            risk_score += 0.08;
            prediction.reason_codes.push_back("PID_GPU_GROWTH_HIGH");
            break;
        }
    }

    // Memory PSI signals
    if (sample.mem_full_avg10 >= 2.0) {
        risk_score += 0.08;
        prediction.reason_codes.push_back("MEM_FULL_PSI_RISING");
    }
    if (sample.mem_full_avg10 >= 5.0) {
        risk_score += 0.12;
        prediction.reason_codes.push_back("MEM_FULL_PSI_CRITICAL");
    }

    // Sustained mem PSI residency (multiple consecutive cycles)
    if (mem_psi_elevated_cycles_ >= 6) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("MEM_PSI_SUSTAINED");
    }

    // IO PSI signal (Tier B/C)
    if (sample.io_full_avg10 >= 1.0) {
        risk_score += 0.05;
        prediction.reason_codes.push_back("IO_FULL_PSI_ELEVATED");
    }
    if (io_psi_elevated_cycles_ >= 6) {
        risk_score += 0.05;
        prediction.reason_codes.push_back("IO_PSI_SUSTAINED");
    }

    // Multi-process GPU contention
    if (prediction.target_pids.size() > 1 && sample.gpu_util_pct >= 80.0) {
        risk_score += 0.08;
        prediction.reason_codes.push_back("MULTI_PROC_GPU_CONTENTION");
    }

    // Sustained critical band residency (5+ cycles ~ 2.5s at 500ms cadence)
    if (critical_band_cycles_ >= 5) {
        risk_score += 0.10;
        prediction.reason_codes.push_back("CRITICAL_BAND_SUSTAINED");
    }

    prediction.risk_score = clamp01(risk_score);

    // Lead time estimate: how many seconds before VRAM runs out at current slope
    if (vram_slope_mb_s > 0.0 && sample.vram_free_mb > 0.0) {
        prediction.lead_time_s = std::max(0.0, sample.vram_free_mb / vram_slope_mb_s);
    } else if (vram_headroom_pct <= 10.0) {
        // No slope data but headroom is very low — estimate conservatively
        prediction.lead_time_s = 3.0;
    }

    // Predicted event classification
    const bool vram_driven = vram_headroom_pct <= 10.0 || vram_slope_mb_s >= 50.0;
    const bool mixed_pressure = sample.mem_full_avg10 >= 2.0 && score.band == PressureBand::Critical;
    const bool io_pressure = io_psi_elevated_cycles_ >= 4;

    if (mixed_pressure) {
        prediction.predicted_event = "mixed_pressure_collapse";
    } else if (vram_driven) {
        prediction.predicted_event = "gpu_oom";
    } else if (io_pressure) {
        prediction.predicted_event = "io_latency_spike";
    } else if (sample.mem_full_avg10 >= 1.0 || sample.cpu_full_avg10 >= 1.0) {
        prediction.predicted_event = "latency_spike";
    } else {
        prediction.predicted_event = "none";
    }

    // Risk band and recommended action
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

    // Track high_risk_cycles for future use
    if (prediction.risk_band == RiskBand::High || prediction.risk_band == RiskBand::Critical) {
        ++high_risk_cycles_;
    } else {
        high_risk_cycles_ = 0;
    }

    if (prediction.reason_codes.empty()) {
        prediction.reason_codes.push_back("PRESSURE_STABLE");
    }

    return prediction;
}

} // namespace hermes
