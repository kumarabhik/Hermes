// hermes_eval: Offline predictor quality evaluator.
//
// Reads predictions.ndjson and events.ndjson from a run directory and computes:
//   - true positive (TP): prediction risk_band>=high AND an oom/collapse event occurred within lead_time_s
//   - false positive (FP): prediction risk_band>=high but no event followed
//   - false negative (FN): an oom/collapse event occurred with no prior high-risk prediction
//   - precision = TP / (TP + FP)
//   - recall    = TP / (TP + FN)
//   - F1        = 2 * precision * recall / (precision + recall)
//   - mean lead time (seconds) across all TP predictions
//
// Output: eval_summary.json in the run directory.
//
// Usage:
//   hermes_eval <run_directory>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- Minimal JSON field extraction helpers ----

// Extract a string value for a given key in a flat JSON line.
// Only handles simple {"key":"value"} patterns (no nesting within key value).
std::string extract_string(const std::string& line, const std::string& key) {
    const std::string search = "\"" + key + "\":\"";
    const auto pos = line.find(search);
    if (pos == std::string::npos) return "";
    const auto start = pos + search.size();
    const auto end = line.find('"', start);
    if (end == std::string::npos) return "";
    return line.substr(start, end - start);
}

double extract_double(const std::string& line, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = line.find(search);
    if (pos == std::string::npos) return 0.0;
    const auto start = pos + search.size();
    try {
        return std::stod(line.substr(start));
    } catch (...) {
        return 0.0;
    }
}

uint64_t extract_uint64(const std::string& line, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = line.find(search);
    if (pos == std::string::npos) return 0;
    const auto start = pos + search.size();
    try {
        return std::stoull(line.substr(start));
    } catch (...) {
        return 0;
    }
}

// ---- Record types ----

struct PredictionRecord {
    uint64_t ts_mono{0};
    std::string risk_band;
    double risk_score{0.0};
    std::string predicted_event;
    double lead_time_s{0.0};
};

struct EventRecord {
    uint64_t ts_mono{0};
    std::string kind;
};

bool is_high_risk(const std::string& risk_band) {
    return risk_band == "high" || risk_band == "critical";
}

bool is_failure_event(const std::string& kind) {
    // Events that represent actual failures or near-failures in the system.
    return kind == "gpu_oom" || kind == "oom_kill" || kind == "memory_collapse"
        || kind == "latency_breach" || kind == "process_crash";
}

// ---- File readers ----

std::vector<PredictionRecord> read_predictions(const std::string& path) {
    std::vector<PredictionRecord> records;
    std::ifstream file(path);
    if (!file.is_open()) return records;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        PredictionRecord rec;
        rec.ts_mono = extract_uint64(line, "ts_mono");
        rec.risk_band = extract_string(line, "risk_band");
        rec.risk_score = extract_double(line, "risk_score");
        rec.predicted_event = extract_string(line, "predicted_event");
        rec.lead_time_s = extract_double(line, "lead_time_s");
        records.push_back(rec);
    }
    return records;
}

std::vector<EventRecord> read_events(const std::string& path) {
    std::vector<EventRecord> records;
    std::ifstream file(path);
    if (!file.is_open()) return records;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        EventRecord rec;
        rec.ts_mono = extract_uint64(line, "ts_mono");
        rec.kind = extract_string(line, "kind");
        records.push_back(rec);
    }
    return records;
}

// ---- Evaluation logic ----

struct EvalResult {
    int true_positives{0};
    int false_positives{0};
    int false_negatives{0};
    double precision{0.0};
    double recall{0.0};
    double f1{0.0};
    double mean_lead_time_s{0.0};
    int total_high_risk_predictions{0};
    int total_failure_events{0};
};

EvalResult evaluate(
    const std::vector<PredictionRecord>& predictions,
    const std::vector<EventRecord>& events,
    double match_window_s) {

    EvalResult result;
    const uint64_t window_ms = static_cast<uint64_t>(match_window_s * 1000.0);

    // Collect failure event timestamps
    std::vector<uint64_t> failure_ts;
    for (const auto& ev : events) {
        if (is_failure_event(ev.kind)) {
            failure_ts.push_back(ev.ts_mono);
            ++result.total_failure_events;
        }
    }
    std::sort(failure_ts.begin(), failure_ts.end());

    // For each high-risk prediction, check if a failure event follows within lead_time_s
    std::vector<bool> failure_matched(failure_ts.size(), false);

    for (const auto& pred : predictions) {
        if (!is_high_risk(pred.risk_band)) continue;
        ++result.total_high_risk_predictions;

        const uint64_t pred_end = pred.ts_mono + window_ms;
        bool matched = false;

        for (std::size_t i = 0; i < failure_ts.size(); ++i) {
            if (failure_ts[i] >= pred.ts_mono && failure_ts[i] <= pred_end) {
                matched = true;
                failure_matched[i] = true;
                const double lead = static_cast<double>(failure_ts[i] - pred.ts_mono) / 1000.0;
                result.mean_lead_time_s += lead;
                break;
            }
        }

        if (matched) {
            ++result.true_positives;
        } else {
            ++result.false_positives;
        }
    }

    // Count unmatched failure events as false negatives
    for (std::size_t i = 0; i < failure_matched.size(); ++i) {
        if (!failure_matched[i]) {
            ++result.false_negatives;
        }
    }

    if (result.true_positives > 0) {
        result.mean_lead_time_s /= static_cast<double>(result.true_positives);
    }

    const double denom_p = result.true_positives + result.false_positives;
    const double denom_r = result.true_positives + result.false_negatives;

    result.precision = denom_p > 0.0 ? result.true_positives / denom_p : 0.0;
    result.recall = denom_r > 0.0 ? result.true_positives / denom_r : 0.0;

    if (result.precision + result.recall > 0.0) {
        result.f1 = 2.0 * result.precision * result.recall / (result.precision + result.recall);
    }

    return result;
}

// ---- JSON output ----

std::string json_escape(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else oss << c;
    }
    return oss.str();
}

void write_eval_summary(
    const std::string& path,
    const std::string& run_dir,
    const EvalResult& result,
    double match_window_s) {

    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "hermes_eval: failed to write " << path << std::endl;
        return;
    }

    out << "{\n"
        << "  \"run_directory\": \"" << json_escape(run_dir) << "\",\n"
        << "  \"match_window_s\": " << match_window_s << ",\n"
        << "  \"total_high_risk_predictions\": " << result.total_high_risk_predictions << ",\n"
        << "  \"total_failure_events\": " << result.total_failure_events << ",\n"
        << "  \"true_positives\": " << result.true_positives << ",\n"
        << "  \"false_positives\": " << result.false_positives << ",\n"
        << "  \"false_negatives\": " << result.false_negatives << ",\n"
        << "  \"precision\": " << result.precision << ",\n"
        << "  \"recall\": " << result.recall << ",\n"
        << "  \"f1\": " << result.f1 << ",\n"
        << "  \"mean_lead_time_s\": " << result.mean_lead_time_s << "\n"
        << "}\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: hermes_eval <run_directory> [match_window_s]\n"
                  << "  run_directory   Path to a hermesd run artifact directory.\n"
                  << "  match_window_s  Seconds within which a failure must follow a prediction (default: 10)\n";
        return 1;
    }

    const std::string run_dir = argv[1];
    const double match_window_s = (argc >= 3) ? std::stod(argv[2]) : 10.0;

    const std::string predictions_path = run_dir + "/predictions.ndjson";
    const std::string events_path = run_dir + "/events.ndjson";
    const std::string output_path = run_dir + "/eval_summary.json";

    const auto predictions = read_predictions(predictions_path);
    const auto events = read_events(events_path);

    if (predictions.empty()) {
        std::cerr << "hermes_eval: no prediction records found in " << predictions_path << std::endl;
        return 1;
    }

    std::cout << "hermes_eval: loaded " << predictions.size() << " predictions, "
              << events.size() << " events from " << run_dir << std::endl;

    const EvalResult result = evaluate(predictions, events, match_window_s);

    std::cout << "  high-risk predictions : " << result.total_high_risk_predictions << "\n"
              << "  failure events        : " << result.total_failure_events << "\n"
              << "  true positives  (TP)  : " << result.true_positives << "\n"
              << "  false positives (FP)  : " << result.false_positives << "\n"
              << "  false negatives (FN)  : " << result.false_negatives << "\n"
              << "  precision             : " << result.precision << "\n"
              << "  recall                : " << result.recall << "\n"
              << "  F1                    : " << result.f1 << "\n"
              << "  mean lead time (s)    : " << result.mean_lead_time_s << "\n";

    write_eval_summary(output_path, run_dir, result, match_window_s);
    std::cout << "hermes_eval: wrote " << output_path << std::endl;

    return 0;
}
