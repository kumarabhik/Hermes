// hermes_simulate: full daemon pipeline simulation from saved NDJSON samples.
//
// Feeds a samples.ndjson file through the real PressureScoreCalculator,
// OomPredictor, Scheduler, and DryRunExecutor stack and writes a fresh set of
// run artifacts — without requiring a live Linux kernel, GPU, or PSI support.
//
// This enables:
//   - Windows-native end-to-end pipeline testing using any saved sample set
//   - Config-change impact assessment: re-simulate the same samples under a
//     different schema.yaml to see how decisions would differ
//   - Fixture-based regression testing across scheduler versions
//
// Usage:
//   hermes_simulate <samples.ndjson>
//   hermes_simulate <run-dir>                        # reads run-dir/samples.ndjson
//   hermes_simulate <samples.ndjson> --config <yaml>
//   hermes_simulate <samples.ndjson> --out <run-dir>
//   hermes_simulate <samples.ndjson> --compare <original-run-dir>
//
// Without --out, writes artifacts to artifacts/logs/sim-<timestamp>/.
// With --compare, prints a diff table of decision match rates vs original.

#include "hermes/core/types.hpp"
#include "hermes/engine/predictor.hpp"
#include "hermes/engine/pressure_score.hpp"
#include "hermes/engine/scheduler.hpp"
#include "hermes/actions/dry_run_executor.hpp"
#include "hermes/runtime/event_logger.hpp"
#include "hermes/runtime/run_metadata.hpp"
#include "hermes/runtime/telemetry_quality.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- Flat JSON field extractors ----

std::string jstr(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = j.find(kk);
    if (p == std::string::npos) return "";
    const auto s = p + kk.size(), e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
double jdbl(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0.0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return 0.0;
    try { return std::stod(j.substr(s)); } catch (...) { return 0.0; }
}
uint64_t jull(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return 0;
    try { return std::stoull(j.substr(s)); } catch (...) { return 0; }
}

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

// ---- Decision/score serialisers matching hermesd output format ----

std::string score_to_json(const std::string& run_id, const hermes::PressureScore& sc) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(4);
    o << "{\"run_id\":\"" << run_id << "\","
      << "\"ts_mono\":" << sc.ts_mono << ","
      << "\"ups\":" << sc.ups << ","
      << "\"pressure_band\":\"" << hermes::to_string(sc.band) << "\","
      << "\"band_changed\":" << (sc.band_changed ? "true" : "false") << ","
      << "\"weighted_cpu\":" << sc.components.weighted_cpu << ","
      << "\"weighted_mem\":" << sc.components.weighted_mem << ","
      << "\"weighted_gpu_util\":" << sc.components.weighted_gpu_util << ","
      << "\"weighted_vram\":" << sc.components.weighted_vram << ","
      << "\"weighted_io\":" << sc.components.weighted_io
      << "}";
    return o.str();
}

std::string risk_to_json(const std::string& run_id, const hermes::RiskPrediction& r) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(4);
    o << "{\"run_id\":\"" << run_id << "\","
      << "\"ts_mono\":" << r.ts_mono << ","
      << "\"risk_score\":" << r.risk_score << ","
      << "\"risk_band\":\"" << hermes::to_string(r.risk_band) << "\","
      << "\"predicted_event\":\"" << r.predicted_event << "\","
      << "\"lead_time_s\":" << r.lead_time_s
      << "}";
    return o.str();
}

std::string decision_to_json(const std::string& run_id, const hermes::InterventionDecision& d) {
    std::ostringstream o;
    o << "{\"run_id\":\"" << run_id << "\","
      << "\"ts_mono\":" << d.ts_mono << ","
      << "\"level\":\"" << hermes::to_string(d.level) << "\","
      << "\"action\":\"" << hermes::to_string(d.action) << "\","
      << "\"scheduler_state\":\"" << hermes::to_string(d.scheduler_state) << "\","
      << "\"state_changed\":" << (d.scheduler_state_changed ? "true" : "false") << ","
      << "\"should_execute\":" << (d.should_execute ? "true" : "false") << ","
      << "\"why\":\"" << d.why << "\""
      << "}";
    return o.str();
}

// ---- Compare simulated decisions against an original run ----

struct MatchStats {
    uint64_t total{0};
    uint64_t action_match{0};
    uint64_t state_match{0};
    uint64_t level_match{0};
};

MatchStats compare_to_original(const std::vector<hermes::InterventionDecision>& sim,
                                const std::filesystem::path& orig_dir) {
    MatchStats ms;
    const std::filesystem::path dec_path = orig_dir / "decisions.ndjson";
    std::ifstream f(dec_path);
    if (!f.is_open()) return ms;

    std::string line;
    std::size_t idx = 0;
    while (std::getline(f, line)) {
        if (line.empty() || idx >= sim.size()) break;
        const auto& d = sim[idx++];
        ms.total++;
        if (jstr(line, "action") == hermes::to_string(d.action)) ms.action_match++;
        if (jstr(line, "scheduler_state") == hermes::to_string(d.scheduler_state)) ms.state_match++;
        if (jstr(line, "level") == hermes::to_string(d.level)) ms.level_match++;
    }
    return ms;
}

bool has_arg(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a) if (x == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& a, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || has_arg(args, "--help")) {
        std::cout <<
            "Usage: hermes_simulate <samples.ndjson|run-dir> [options]\n"
            "  --config  <yaml>     Schema config (default: config/schema.yaml)\n"
            "  --out     <dir>      Output run directory (default: artifacts/logs/sim-<ts>)\n"
            "  --compare <run-dir>  Print decision match table vs original run\n"
            "  --quiet              Suppress per-frame progress output\n";
        return 0;
    }

    // Resolve samples.ndjson path.
    std::filesystem::path samples_path = args[0];
    if (std::filesystem::is_directory(samples_path))
        samples_path = samples_path / "samples.ndjson";

    if (!std::filesystem::exists(samples_path)) {
        std::cerr << "hermes_simulate: cannot find " << samples_path << "\n";
        return 1;
    }

    const std::string config_path = get_arg(args, "--config",
        []() { const char* e = std::getenv("HERMES_CONFIG_PATH");
               return (e && e[0]) ? std::string(e) : "config/schema.yaml"; }());

    // Build output directory.
    const long long ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string default_out = "artifacts/logs/sim-" + std::to_string(ts);
    const std::filesystem::path out_dir = get_arg(args, "--out", default_out);
    std::filesystem::create_directories(out_dir);

    const std::string compare_dir = get_arg(args, "--compare", "");
    const bool quiet = has_arg(args, "--quiet");

    // Build a run_id from the output directory name.
    const std::string run_id = out_dir.filename().string();

    std::cout << "hermes_simulate\n";
    std::cout << "  Input   : " << samples_path << "\n";
    std::cout << "  Config  : " << config_path  << "\n";
    std::cout << "  Output  : " << out_dir      << "\n\n";

    // Open output artifact streams.
    std::ofstream f_scores   (out_dir / "scores.ndjson");
    std::ofstream f_preds    (out_dir / "predictions.ndjson");
    std::ofstream f_decisions(out_dir / "decisions.ndjson");

    // Initialise real pipeline components.
    hermes::PressureScoreCalculator scorer;
    hermes::OomPredictor predictor;
    hermes::SchedulerConfig sched_cfg;
    sched_cfg.mode = hermes::OperatingMode::ObserveOnly;
    hermes::Scheduler scheduler(sched_cfg);
    hermes::DryRunExecutor executor;

    // Process samples.
    std::ifstream f_in(samples_path);
    std::string line;
    uint64_t frame = 0;

    // Accumulators for telemetry_quality.
    double peak_ups = 0.0, peak_risk = 0.0;
    uint64_t level1_count = 0, level2_count = 0, level3_count = 0;
    std::vector<hermes::InterventionDecision> all_decisions;

    while (std::getline(f_in, line)) {
        if (line.empty()) continue;
        ++frame;

        const hermes::PressureSample sample = parse_sample(line);
        const hermes::PressureScore  score  = scorer.compute(sample);
        const hermes::RiskPrediction risk   = predictor.predict(sample, {});
        const hermes::InterventionDecision decision =
            scheduler.evaluate(score, risk, {});
        executor.execute(decision);

        f_scores    << score_to_json(run_id, score)    << "\n";
        f_preds     << risk_to_json(run_id, risk)      << "\n";
        f_decisions << decision_to_json(run_id, decision) << "\n";

        if (score.ups > peak_ups)  peak_ups  = score.ups;
        if (risk.risk_score > peak_risk) peak_risk = risk.risk_score;
        if (decision.level == hermes::ActionLevel::Level1) ++level1_count;
        if (decision.level == hermes::ActionLevel::Level2) ++level2_count;
        if (decision.level == hermes::ActionLevel::Level3) ++level3_count;

        all_decisions.push_back(decision);

        if (!quiet && frame % 50 == 0)
            std::cout << "  frame " << frame << "  ups=" << std::fixed
                      << std::setprecision(1) << score.ups
                      << "  state=" << hermes::to_string(decision.scheduler_state) << "\n";
    }

    f_scores.close(); f_preds.close(); f_decisions.close();

    // Write run_metadata.json.
    {
        std::ofstream mf(out_dir / "run_metadata.json");
        mf << "{\"run_id\":\"" << run_id << "\","
           << "\"mode\":\"simulate\","
           << "\"samples_source\":\"" << samples_path.string() << "\","
           << "\"config_path\":\"" << config_path << "\","
           << "\"ts_wall\":" << ts << "}";
    }

    // Write telemetry_quality.json.
    {
        std::ofstream tq(out_dir / "telemetry_quality.json");
        tq << std::fixed << std::setprecision(4);
        tq << "{\"run_id\":\"" << run_id << "\","
           << "\"total_frames\":" << frame << ","
           << "\"peak_ups\":" << peak_ups << ","
           << "\"peak_risk\":" << peak_risk << ","
           << "\"level1_count\":" << level1_count << ","
           << "\"level2_count\":" << level2_count << ","
           << "\"level3_count\":" << level3_count << "}";
    }

    const std::string sep(60, '=');
    std::cout << "\n" << sep << "\n";
    std::cout << "hermes_simulate complete\n";
    std::cout << sep << "\n";
    std::cout << "  Frames    : " << frame << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Peak UPS  : " << peak_ups << "\n";
    std::cout << "  Peak Risk : " << peak_risk << "\n";
    std::cout << "  L1 actions: " << level1_count << "\n";
    std::cout << "  L2 actions: " << level2_count << "\n";
    std::cout << "  L3 actions: " << level3_count << "\n";
    std::cout << "  Artifacts : " << out_dir << "\n";

    // Optional comparison vs original run.
    if (!compare_dir.empty()) {
        const MatchStats ms = compare_to_original(all_decisions, compare_dir);
        std::cout << "\n--- Comparison vs " << compare_dir << " ---\n";
        if (ms.total == 0) {
            std::cout << "  (no matching decisions.ndjson found)\n";
        } else {
            auto pct = [&](uint64_t n) {
                return std::to_string(static_cast<int>(100.0 * n / ms.total)) + "%";
            };
            std::cout << "  Frames compared : " << ms.total << "\n";
            std::cout << "  Action match    : " << pct(ms.action_match)
                      << " (" << ms.action_match << "/" << ms.total << ")\n";
            std::cout << "  State match     : " << pct(ms.state_match)
                      << " (" << ms.state_match  << "/" << ms.total << ")\n";
            std::cout << "  Level match     : " << pct(ms.level_match)
                      << " (" << ms.level_match  << "/" << ms.total << ")\n";
        }
    }
    std::cout << sep << "\n";

    std::cout << "\nNext steps:\n";
    std::cout << "  hermes_replay " << out_dir.string() << "\n";
    std::cout << "  hermes_eval   " << out_dir.string() << "\n";
    if (!compare_dir.empty())
        std::cout << "  hermesctl diff " << compare_dir << " " << out_dir.string() << "\n";
    return 0;
}
