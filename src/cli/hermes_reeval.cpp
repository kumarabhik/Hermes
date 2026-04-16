// hermes_reeval: Deterministic replay re-execution.
//
// Reads samples.ndjson from a run directory, feeds each sample through
// the real PressureScoreCalculator + OomPredictor + Scheduler pipeline,
// and compares each replayed decision to the original decisions.ndjson.
//
// Writes replay_eval.ndjson with per-frame match/mismatch results and
// prints an aggregate summary (action match rate, state match rate,
// scheduler state transition coverage).
//
// Also writes state_coverage.json listing which scheduler states and
// transitions were visited during replay — useful for verifying that a
// fixture or run exercises the expected state-machine paths.
//
// NOTE: Process-level data (processes.ndjson) is not replayed in this
// version — OomPredictor receives an empty process list. UPS, risk score,
// and scheduler-state accuracy are therefore the primary comparison axes.
//
// Usage:
//   hermes_reeval <run-directory>
//   hermes_reeval <run-directory> --out path/to/replay_eval.ndjson

#include "hermes/core/types.hpp"
#include "hermes/engine/predictor.hpp"
#include "hermes/engine/pressure_score.hpp"
#include "hermes/engine/scheduler.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- Minimal flat-JSON field extractors ----

std::string jstr(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":\"";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    const auto start = pos + search.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

double jdbl(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;
    const auto start = pos + search.size();
    // Skip over any leading quote (string field) — only parse bare numbers
    if (start < json.size() && json[start] == '"') return 0.0;
    try { return std::stod(json.substr(start)); } catch (...) { return 0.0; }
}

uint64_t jull(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    const auto start = pos + search.size();
    if (start < json.size() && json[start] == '"') return 0;
    try { return std::stoull(json.substr(start)); } catch (...) { return 0; }
}

// ---- Reconstruct PressureSample from a samples.ndjson line ----

hermes::PressureSample parse_sample(const std::string& line) {
    hermes::PressureSample s;
    s.ts_wall           = jull(line, "ts_wall");
    s.ts_mono           = jull(line, "ts_mono");
    s.cpu_some_avg10    = jdbl(line, "cpu_some_avg10");
    s.cpu_full_avg10    = jdbl(line, "cpu_full_avg10");
    s.mem_some_avg10    = jdbl(line, "mem_some_avg10");
    s.mem_full_avg10    = jdbl(line, "mem_full_avg10");
    s.io_some_avg10     = jdbl(line, "io_some_avg10");
    s.io_full_avg10     = jdbl(line, "io_full_avg10");
    s.gpu_util_pct      = jdbl(line, "gpu_util_pct");
    s.vram_used_mb      = jdbl(line, "vram_used_mb");
    s.vram_total_mb     = jdbl(line, "vram_total_mb");
    s.vram_free_mb      = jdbl(line, "vram_free_mb");
    s.loadavg_runnable  = static_cast<uint32_t>(jull(line, "loadavg_runnable"));
    s.vmstat_pgmajfault = jull(line, "vmstat_pgmajfault");
    s.vmstat_pgfault    = jull(line, "vmstat_pgfault");
    return s;
}

// ---------------------------------------------------------------------------
// Scheduler state transition coverage tracker
// ---------------------------------------------------------------------------

static const std::array<hermes::SchedulerState, 5> kAllStates = {{
    hermes::SchedulerState::Normal,
    hermes::SchedulerState::Elevated,
    hermes::SchedulerState::Throttled,
    hermes::SchedulerState::Recovery,
    hermes::SchedulerState::Cooldown
}};

struct StateCoverageTracker {
    std::map<hermes::SchedulerState, uint64_t> state_counts;
    // Transition key: (from_state, to_state) → count  (only actual transitions, not self-loops).
    std::map<std::pair<hermes::SchedulerState, hermes::SchedulerState>, uint64_t> transition_counts;

    hermes::SchedulerState prev_state{hermes::SchedulerState::Normal};
    bool first{true};

    void record(hermes::SchedulerState current) {
        state_counts[current]++;
        if (!first && current != prev_state) {
            transition_counts[{prev_state, current}]++;
        }
        first    = false;
        prev_state = current;
    }

    std::size_t states_visited() const { return state_counts.size(); }

    // Write state_coverage.json to the given directory.
    bool write_json(const std::filesystem::path& dir, std::string& error) const {
        const std::filesystem::path path = dir / "state_coverage.json";
        std::ofstream out(path);
        if (!out.is_open()) {
            error = "cannot write " + path.string();
            return false;
        }

        out << "{\n";

        // States visited.
        out << "  \"states_visited\": " << states_visited() << ",\n";
        out << "  \"states_total\": "   << kAllStates.size() << ",\n";
        out << "  \"state_counts\": {\n";
        for (std::size_t i = 0; i < kAllStates.size(); ++i) {
            const hermes::SchedulerState s = kAllStates[i];
            const auto it = state_counts.find(s);
            const uint64_t cnt = (it != state_counts.end()) ? it->second : 0u;
            out << "    \"" << hermes::to_string(s) << "\": " << cnt;
            out << (i + 1 < kAllStates.size() ? ",\n" : "\n");
        }
        out << "  },\n";

        // Transitions observed.
        out << "  \"transitions_observed\": " << transition_counts.size() << ",\n";
        out << "  \"transitions\": [\n";
        bool first_t = true;
        for (const auto& kv : transition_counts) {
            if (!first_t) out << ",\n";
            out << "    {\"from\":\"" << hermes::to_string(kv.first.first)
                << "\",\"to\":\"" << hermes::to_string(kv.first.second)
                << "\",\"count\":" << kv.second << "}";
            first_t = false;
        }
        if (!transition_counts.empty()) out << "\n";
        out << "  ]\n";

        out << "}\n";
        return out.good();
    }
};

void print_usage() {
    std::cout
        << "Usage: hermes_reeval <run-directory> [--out replay_eval.ndjson]\n"
        << "\n"
        << "Re-executes the pressure scoring, prediction, and scheduling pipeline\n"
        << "against recorded samples.ndjson and compares outputs to decisions.ndjson.\n"
        << "Emits replay_eval.ndjson with per-frame match/mismatch results.\n"
        << "\n"
        << "Options:\n"
        << "  --out <path>   Output path (default: <run-directory>/replay_eval.ndjson)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 ||
        std::string(argv[1]) == "--help" ||
        std::string(argv[1]) == "-h") {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::filesystem::path run_dir = argv[1];
    std::filesystem::path out_path = run_dir / "replay_eval.ndjson";

    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--out") {
            out_path = argv[i + 1];
        }
    }

    const std::filesystem::path samples_path   = run_dir / "samples.ndjson";
    const std::filesystem::path decisions_path = run_dir / "decisions.ndjson";

    // ---- Load recorded decisions line by line ----
    std::vector<std::string> recorded_lines;
    {
        std::ifstream f(decisions_path);
        if (!f.is_open()) {
            std::cerr << "hermes_reeval: cannot open " << decisions_path << "\n";
            return 2;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) recorded_lines.push_back(std::move(line));
        }
        std::cout << "Loaded " << recorded_lines.size()
                  << " recorded decisions from " << decisions_path << "\n";
    }

    // ---- Open samples file ----
    std::ifstream samples_f(samples_path);
    if (!samples_f.is_open()) {
        std::cerr << "hermes_reeval: cannot open " << samples_path << "\n";
        return 2;
    }

    // ---- Ensure output directory exists ----
    try {
        std::filesystem::create_directories(out_path.parent_path());
    } catch (...) {}

    std::ofstream out_f(out_path);
    if (!out_f.is_open()) {
        std::cerr << "hermes_reeval: cannot write " << out_path << "\n";
        return 2;
    }

    // ---- Pipeline instances (stateful — same lifecycle as hermesd) ----
    hermes::PressureScoreCalculator calculator;
    hermes::OomPredictor            predictor;
    hermes::SchedulerConfig         sched_config;
    sched_config.mode = hermes::OperatingMode::ObserveOnly;
    hermes::Scheduler               scheduler(sched_config);

    const std::vector<hermes::ProcessSnapshot> empty_procs;

    // ---- Re-execution loop ----
    uint64_t frame_id     = 0;
    uint64_t compared     = 0;
    uint64_t action_match = 0;
    uint64_t state_match  = 0;
    uint64_t band_match   = 0;

    double ups_sum_sq_err  = 0.0;
    double risk_sum_sq_err = 0.0;
    uint64_t metric_count  = 0;

    StateCoverageTracker coverage;

    std::string sample_line;
    while (std::getline(samples_f, sample_line)) {
        if (sample_line.empty()) continue;

        const hermes::PressureSample      sample   = parse_sample(sample_line);
        const hermes::PressureScore       score    = calculator.compute(sample);
        const hermes::RiskPrediction      pred     = predictor.update(sample, empty_procs, score);
        const hermes::InterventionDecision decision = scheduler.evaluate(score, pred, empty_procs);

        const std::string replayed_action = hermes::to_string(decision.action);
        const std::string replayed_state  = hermes::to_string(decision.scheduler_state);
        const std::string replayed_band   = hermes::to_string(score.band);

        coverage.record(decision.scheduler_state);

        // Compare against recorded if available
        const bool has_recorded = (frame_id < recorded_lines.size());
        std::string recorded_action = "n/a";
        std::string recorded_state  = "n/a";
        std::string recorded_band   = "n/a";
        double recorded_ups  = 0.0;
        double recorded_risk = 0.0;

        bool action_ok = false;
        bool state_ok  = false;
        bool band_ok   = false;

        if (has_recorded) {
            const std::string& rec = recorded_lines[static_cast<std::size_t>(frame_id)];
            recorded_action = jstr(rec, "action");
            recorded_state  = jstr(rec, "state");
            recorded_band   = jstr(rec, "pressure_band");
            recorded_ups    = jdbl(rec, "ups");
            recorded_risk   = jdbl(rec, "risk_score");

            action_ok = (recorded_action == replayed_action);
            state_ok  = (recorded_state  == replayed_state);
            band_ok   = (recorded_band   == replayed_band);

            if (action_ok) ++action_match;
            if (state_ok)  ++state_match;
            if (band_ok)   ++band_match;

            const double ups_err  = score.ups      - recorded_ups;
            const double risk_err = pred.risk_score - recorded_risk;
            ups_sum_sq_err  += ups_err  * ups_err;
            risk_sum_sq_err += risk_err * risk_err;
            ++metric_count;
            ++compared;
        }

        // Write eval record
        out_f << "{\"frame_id\":"    << frame_id
              << ",\"sample_ts_mono\":" << sample.ts_mono
              << ",\"recorded_action\":\"" << recorded_action << "\""
              << ",\"replayed_action\":\"" << replayed_action << "\""
              << ",\"recorded_state\":\"" << recorded_state << "\""
              << ",\"replayed_state\":\"" << replayed_state << "\""
              << ",\"recorded_band\":\"" << recorded_band << "\""
              << ",\"replayed_band\":\"" << replayed_band << "\""
              << ",\"action_match\":"    << (action_ok ? "true" : "false")
              << ",\"state_match\":"     << (state_ok  ? "true" : "false")
              << ",\"band_match\":"      << (band_ok   ? "true" : "false")
              << ",\"replayed_ups\":"    << std::fixed << std::setprecision(2) << score.ups
              << ",\"replayed_risk\":"   << std::fixed << std::setprecision(3) << pred.risk_score
              << ",\"recorded_ups\":"    << std::fixed << std::setprecision(2) << recorded_ups
              << ",\"recorded_risk\":"   << std::fixed << std::setprecision(3) << recorded_risk
              << "}\n";

        ++frame_id;
    }
    out_f.flush();

    // ---- Print summary ----
    const auto pct = [](uint64_t n, uint64_t d) -> double {
        return d == 0 ? 0.0 : 100.0 * static_cast<double>(n) / static_cast<double>(d);
    };

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n=== hermes_reeval Summary ===\n";
    std::cout << "Frames replayed : " << frame_id << "\n";
    std::cout << "Frames compared : " << compared << "\n";
    if (compared > 0) {
        std::cout << "Action match    : " << action_match << "/" << compared
                  << " (" << pct(action_match, compared) << "%)\n";
        std::cout << "State match     : " << state_match  << "/" << compared
                  << " (" << pct(state_match,  compared) << "%)\n";
        std::cout << "Band match      : " << band_match   << "/" << compared
                  << " (" << pct(band_match,   compared) << "%)\n";
    }
    if (metric_count > 0) {
        const double ups_rmse  = std::sqrt(ups_sum_sq_err  / static_cast<double>(metric_count));
        const double risk_rmse = std::sqrt(risk_sum_sq_err / static_cast<double>(metric_count));
        std::cout << std::setprecision(3);
        std::cout << "UPS RMSE        : " << ups_rmse  << "\n";
        std::cout << "Risk RMSE       : " << risk_rmse << "\n";
    }
    std::cout << "Output          : " << out_path.string() << "\n";

    // ---- State coverage ----
    std::cout << "\n=== Scheduler State Coverage ===\n";
    std::cout << "States visited  : " << coverage.states_visited()
              << "/" << kAllStates.size() << "\n";
    for (const hermes::SchedulerState s : kAllStates) {
        const auto it = coverage.state_counts.find(s);
        const uint64_t cnt = (it != coverage.state_counts.end()) ? it->second : 0u;
        std::cout << "  " << hermes::to_string(s) << ": " << cnt << " frames\n";
    }
    std::cout << "Transitions     : " << coverage.transition_counts.size() << " unique\n";
    for (const auto& kv : coverage.transition_counts) {
        std::cout << "  " << hermes::to_string(kv.first.first)
                  << " -> " << hermes::to_string(kv.first.second)
                  << " : " << kv.second << "\n";
    }

    std::string cov_error;
    if (coverage.write_json(run_dir, cov_error)) {
        std::cout << "Coverage JSON   : " << (run_dir / "state_coverage.json").string() << "\n";
    } else {
        std::cerr << "hermes_reeval: warning: could not write state_coverage.json: "
                  << cov_error << "\n";
    }

    return 0;
}
