#include "hermes/replay/replay_summary.hpp"

#include "hermes/runtime/event_logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace hermes {
namespace {

bool extract_raw_value(const std::string& line, const std::string& key, std::string& value) {
    const std::string token = "\"" + key + "\":";
    std::size_t pos = line.find(token);
    if (pos == std::string::npos) {
        return false;
    }

    pos += token.size();
    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }

    if (pos >= line.size()) {
        return false;
    }

    if (line[pos] == '"') {
        ++pos;
        std::ostringstream oss;
        bool escaped = false;
        for (; pos < line.size(); ++pos) {
            const char ch = line[pos];
            if (escaped) {
                switch (ch) {
                case 'n':
                    oss << '\n';
                    break;
                case 'r':
                    oss << '\r';
                    break;
                case 't':
                    oss << '\t';
                    break;
                case 'b':
                    oss << '\b';
                    break;
                case 'f':
                    oss << '\f';
                    break;
                default:
                    oss << ch;
                    break;
                }
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                value = oss.str();
                return true;
            }
            oss << ch;
        }
        return false;
    }

    std::size_t end = pos;
    int nesting = 0;
    while (end < line.size()) {
        const char ch = line[end];
        if (ch == '[' || ch == '{') {
            ++nesting;
        } else if (ch == ']' || ch == '}') {
            if (nesting == 0) {
                break;
            }
            --nesting;
        } else if (ch == ',' && nesting == 0) {
            break;
        }
        ++end;
    }

    value = line.substr(pos, end - pos);
    return !value.empty();
}

bool extract_string(const std::string& line, const std::string& key, std::string& value) {
    return extract_raw_value(line, key, value);
}

bool extract_double(const std::string& line, const std::string& key, double& value) {
    std::string raw;
    if (!extract_raw_value(line, key, raw)) {
        return false;
    }

    try {
        value = std::stod(raw);
        return true;
    } catch (...) {
        return false;
    }
}

bool extract_uint64(const std::string& line, const std::string& key, uint64_t& value) {
    std::string raw;
    if (!extract_raw_value(line, key, raw)) {
        return false;
    }

    try {
        value = static_cast<uint64_t>(std::stoull(raw));
        return true;
    } catch (...) {
        return false;
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool parse_json_string_array(const std::string& raw, std::vector<std::string>& values) {
    values.clear();

    std::size_t pos = raw.find('[');
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;

    while (pos < raw.size()) {
        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t' || raw[pos] == '\r' || raw[pos] == '\n' || raw[pos] == ',')) {
            ++pos;
        }

        if (pos >= raw.size()) {
            return false;
        }
        if (raw[pos] == ']') {
            return true;
        }
        if (raw[pos] != '"') {
            return false;
        }

        ++pos;
        std::ostringstream value;
        bool escaped = false;
        for (; pos < raw.size(); ++pos) {
            const char ch = raw[pos];
            if (escaped) {
                switch (ch) {
                case 'n':
                    value << '\n';
                    break;
                case 'r':
                    value << '\r';
                    break;
                case 't':
                    value << '\t';
                    break;
                case 'b':
                    value << '\b';
                    break;
                case 'f':
                    value << '\f';
                    break;
                default:
                    value << ch;
                    break;
                }
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                values.push_back(value.str());
                ++pos;
                break;
            }
            value << ch;
        }
    }

    return false;
}

void skip_json_space(const std::string& raw, std::size_t& pos) {
    while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos])) != 0) {
        ++pos;
    }
}

bool parse_json_quoted_string(const std::string& raw, std::size_t& pos, std::string& value) {
    if (pos >= raw.size() || raw[pos] != '"') {
        return false;
    }

    ++pos;
    std::ostringstream oss;
    bool escaped = false;
    for (; pos < raw.size(); ++pos) {
        const char ch = raw[pos];
        if (escaped) {
            switch (ch) {
            case 'n':
                oss << '\n';
                break;
            case 'r':
                oss << '\r';
                break;
            case 't':
                oss << '\t';
                break;
            case 'b':
                oss << '\b';
                break;
            case 'f':
                oss << '\f';
                break;
            default:
                oss << ch;
                break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            ++pos;
            value = oss.str();
            return true;
        }
        oss << ch;
    }

    return false;
}

bool parse_json_number_object(const std::string& raw, std::map<std::string, double>& values) {
    values.clear();

    std::size_t pos = raw.find('{');
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;

    while (pos < raw.size()) {
        skip_json_space(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') {
            ++pos;
            continue;
        }
        skip_json_space(raw, pos);

        if (pos >= raw.size()) {
            return false;
        }
        if (raw[pos] == '}') {
            return true;
        }

        std::string key;
        if (!parse_json_quoted_string(raw, pos, key)) {
            return false;
        }

        skip_json_space(raw, pos);
        if (pos >= raw.size() || raw[pos] != ':') {
            return false;
        }
        ++pos;
        skip_json_space(raw, pos);

        const std::size_t value_start = pos;
        while (pos < raw.size()) {
            const char ch = raw[pos];
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
                ++pos;
                continue;
            }
            break;
        }

        if (value_start == pos) {
            return false;
        }

        try {
            values[key] = std::stod(raw.substr(value_start, pos - value_start));
        } catch (...) {
            return false;
        }
    }

    return false;
}

bool parse_json_count_object(const std::string& raw, std::map<std::string, std::size_t>& values) {
    values.clear();

    std::map<std::string, double> numbers;
    if (!parse_json_number_object(raw, numbers)) {
        return false;
    }

    for (const auto& [key, value] : numbers) {
        if (value < 0.0) {
            return false;
        }
        values[key] = static_cast<std::size_t>(value);
    }

    return true;
}

void observe_signal(ReplaySummary& summary, const std::string& signal) {
    if (!signal.empty()) {
        ++summary.observed_signals[signal];
    }
}

void observe_time_range(ReplaySummary& summary, uint64_t ts_wall, uint64_t ts_mono) {
    if (ts_wall != 0 && (summary.first_ts_wall == 0 || ts_wall < summary.first_ts_wall)) {
        summary.first_ts_wall = ts_wall;
    }
    if (ts_wall > summary.last_ts_wall) {
        summary.last_ts_wall = ts_wall;
    }
    if (ts_mono != 0 && (summary.first_ts_mono == 0 || ts_mono < summary.first_ts_mono)) {
        summary.first_ts_mono = ts_mono;
    }
    if (ts_mono > summary.last_ts_mono) {
        summary.last_ts_mono = ts_mono;
    }
}

void observe_identity(
    ReplaySummary& summary,
    const std::string& filename,
    std::size_t line_number,
    const std::string& line) {
    std::string run_id;
    std::string scenario;
    std::string config_hash;
    uint64_t ts_wall = 0;
    uint64_t ts_mono = 0;

    if (!extract_string(line, "run_id", run_id) ||
        !extract_string(line, "scenario", scenario) ||
        !extract_string(line, "config_hash", config_hash)) {
        summary.valid = false;
        ++summary.counts.parse_errors;
        summary.warnings.push_back(filename + ":" + std::to_string(line_number) + " missing run identity fields");
        return;
    }

    if (summary.run_id.empty()) {
        summary.run_id = run_id;
    } else if (summary.run_id != run_id) {
        summary.valid = false;
        summary.warnings.push_back(filename + ":" + std::to_string(line_number) + " has mismatched run_id=" + run_id);
    }

    if (summary.scenario.empty()) {
        summary.scenario = scenario;
    } else if (summary.scenario != scenario) {
        summary.valid = false;
        summary.warnings.push_back(filename + ":" + std::to_string(line_number) + " has mismatched scenario=" + scenario);
    }

    if (summary.config_hash.empty()) {
        summary.config_hash = config_hash;
    } else if (summary.config_hash != config_hash) {
        summary.valid = false;
        summary.warnings.push_back(filename + ":" + std::to_string(line_number) + " has mismatched config_hash=" + config_hash);
    }

    extract_uint64(line, "ts_wall", ts_wall);
    extract_uint64(line, "ts_mono", ts_mono);
    observe_time_range(summary, ts_wall, ts_mono);
}

void read_ndjson_file(
    const std::filesystem::path& run_directory,
    const std::string& filename,
    ReplaySummary& summary,
    const std::function<void(const std::string&, std::size_t)>& on_line) {
    const std::filesystem::path path = run_directory / filename;
    std::ifstream file(path);
    if (!file.is_open()) {
        summary.valid = false;
        summary.warnings.push_back("missing artifact file: " + path.string());
        return;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        observe_identity(summary, filename, line_number, line);
        on_line(line, line_number);
    }
}

void increment_if_found(const std::string& line, const std::string& field, std::map<std::string, std::size_t>& counts) {
    std::string value;
    if (extract_string(line, field, value) && !value.empty()) {
        ++counts[value];
    }
}

void observe_peak(const std::string& line, const std::string& field, double& peak) {
    double value = 0.0;
    if (extract_double(line, field, value)) {
        peak = std::max(peak, value);
    }
}

std::string map_json(const std::map<std::string, std::size_t>& values) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << "\"" << json_escape(key) << "\":" << value;
    }
    oss << "}";
    return oss.str();
}

std::string double_map_json(const std::map<std::string, double>& values) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << "\"" << json_escape(key) << "\":" << value;
    }
    oss << "}";
    return oss.str();
}

std::string warnings_json(const std::vector<std::string>& warnings) {
    std::ostringstream oss;
    oss << "[";
    for (std::size_t index = 0; index < warnings.size(); ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << "\"" << json_escape(warnings[index]) << "\"";
    }
    oss << "]";
    return oss.str();
}

std::string string_vector_json(const std::vector<std::string>& values) {
    std::ostringstream oss;
    oss << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << "\"" << json_escape(values[index]) << "\"";
    }
    oss << "]";
    return oss.str();
}

std::string csv_escape(const std::string& value) {
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }

    std::ostringstream oss;
    oss << '"';
    for (const char ch : value) {
        if (ch == '"') {
            oss << "\"\"";
        } else {
            oss << ch;
        }
    }
    oss << '"';
    return oss.str();
}

std::size_t count_for(const std::map<std::string, std::size_t>& values, const std::string& key) {
    const auto it = values.find(key);
    return it == values.end() ? 0 : it->second;
}

const char* bool_csv(bool value) {
    return value ? "true" : "false";
}

void fail_assertion(ReplaySummary& summary, const std::string& message) {
    ++summary.assertions_failed;
    summary.valid = false;
    summary.assertion_failures.push_back(message);
}

void pass_assertion(ReplaySummary& summary) {
    ++summary.assertions_passed;
}

void check_min_count(
    ReplaySummary& summary,
    const std::map<std::string, std::size_t>& observed,
    const std::map<std::string, std::size_t>& expected,
    const std::string& label) {
    for (const auto& [key, minimum] : expected) {
        ++summary.assertions_checked;
        const auto it = observed.find(key);
        const std::size_t actual = it == observed.end() ? 0 : it->second;
        if (actual >= minimum) {
            pass_assertion(summary);
            continue;
        }

        fail_assertion(
            summary,
            "expected at least " + std::to_string(minimum) + " " + label +
                " count for " + key + ", observed " + std::to_string(actual));
    }
}

void check_min_double(
    ReplaySummary& summary,
    double actual,
    double minimum,
    const std::string& label) {
    ++summary.assertions_checked;
    if (actual >= minimum) {
        pass_assertion(summary);
        return;
    }

    std::ostringstream message;
    message << "expected " << label << " >= " << minimum << ", observed " << actual;
    fail_assertion(summary, message.str());
}

} // namespace

ReplaySummary ReplaySummaryBuilder::summarize(const std::filesystem::path& run_directory) {
    ReplaySummary summary;
    if (!std::filesystem::exists(run_directory) || !std::filesystem::is_directory(run_directory)) {
        summary.valid = false;
        summary.warnings.push_back("run directory does not exist: " + run_directory.string());
        return summary;
    }

    inspect_metadata_artifacts(run_directory, summary);
    read_samples(run_directory, summary);
    read_processes(run_directory, summary);
    read_scores(run_directory, summary);
    read_predictions(run_directory, summary);
    read_decisions(run_directory, summary);
    read_actions(run_directory, summary);
    read_events(run_directory, summary);
    validate_scenario_manifest(run_directory, summary);

    if (summary.counts.samples == 0) {
        summary.valid = false;
        summary.warnings.push_back("samples.ndjson contained no samples");
    }
    if (summary.counts.decisions == 0) {
        summary.warnings.push_back("decisions.ndjson contained no decisions");
    }
    if (summary.counts.actions == 0) {
        summary.warnings.push_back("actions.ndjson contained no action results");
    }

    return summary;
}

void ReplaySummaryBuilder::inspect_metadata_artifacts(
    const std::filesystem::path& run_directory,
    ReplaySummary& summary) const {
    const std::filesystem::path metadata_path = run_directory / "run_metadata.json";
    const std::filesystem::path snapshot_path = run_directory / "config_snapshot.yaml";
    const std::filesystem::path telemetry_path = run_directory / "telemetry_quality.json";
    const std::filesystem::path manifest_path = run_directory / "scenario_manifest.json";

    summary.run_metadata_present = std::filesystem::exists(metadata_path);
    summary.config_snapshot_present = std::filesystem::exists(snapshot_path);
    summary.telemetry_quality_present = std::filesystem::exists(telemetry_path);
    summary.scenario_manifest_present = std::filesystem::exists(manifest_path);

    if (summary.run_metadata_present) {
        summary.run_metadata_bytes = std::filesystem::file_size(metadata_path);
    } else {
        summary.warnings.push_back("run_metadata.json is missing");
    }

    if (summary.config_snapshot_present) {
        summary.config_snapshot_bytes = std::filesystem::file_size(snapshot_path);
    } else {
        summary.warnings.push_back("config_snapshot.yaml is missing");
    }

    if (summary.telemetry_quality_present) {
        summary.telemetry_quality_bytes = std::filesystem::file_size(telemetry_path);
    } else {
        summary.warnings.push_back("telemetry_quality.json is missing");
    }

    if (summary.scenario_manifest_present) {
        summary.scenario_manifest_bytes = std::filesystem::file_size(manifest_path);
    }
}

void ReplaySummaryBuilder::validate_scenario_manifest(
    const std::filesystem::path& run_directory,
    ReplaySummary& summary) const {
    if (!summary.scenario_manifest_present) {
        return;
    }

    const std::filesystem::path manifest_path = run_directory / "scenario_manifest.json";
    const std::string manifest = read_text_file(manifest_path);
    if (manifest.empty()) {
        summary.valid = false;
        summary.assertion_failures.push_back("scenario_manifest.json is empty or unreadable");
        return;
    }

    std::string expected_raw;
    if (!extract_raw_value(manifest, "expected_signals", expected_raw) ||
        !parse_json_string_array(expected_raw, summary.expected_signals)) {
        summary.valid = false;
        summary.assertion_failures.push_back("scenario_manifest.json missing parseable expected_signals");
        return;
    }

    std::unordered_set<std::string> seen_expected;
    for (const std::string& expected : summary.expected_signals) {
        if (expected.empty() || seen_expected.count(expected) != 0) {
            continue;
        }
        seen_expected.insert(expected);
        ++summary.assertions_checked;

        if (summary.observed_signals.count(expected) != 0 && summary.observed_signals[expected] > 0) {
            ++summary.assertions_passed;
        } else {
            ++summary.assertions_failed;
            summary.valid = false;
            summary.assertion_failures.push_back("missing expected signal: " + expected);
        }
    }

    std::string raw;
    if (extract_raw_value(manifest, "minimums", raw)) {
        std::map<std::string, double> minimums;
        if (!parse_json_number_object(raw, minimums)) {
            fail_assertion(summary, "scenario_manifest.json has invalid minimums object");
        } else {
            const auto peak_ups = minimums.find("peak_ups");
            if (peak_ups != minimums.end()) {
                summary.has_expected_min_peak_ups = true;
                summary.expected_min_peak_ups = peak_ups->second;
                check_min_double(summary, summary.peak_ups, peak_ups->second, "peak_ups");
            }

            const auto peak_risk = minimums.find("peak_risk_score");
            if (peak_risk != minimums.end()) {
                summary.has_expected_min_peak_risk_score = true;
                summary.expected_min_peak_risk_score = peak_risk->second;
                check_min_double(summary, summary.peak_risk_score, peak_risk->second, "peak_risk_score");
            }
        }
    }

    if (extract_raw_value(manifest, "expected_min_decision_actions", raw)) {
        if (!parse_json_count_object(raw, summary.expected_min_decision_actions)) {
            fail_assertion(summary, "scenario_manifest.json has invalid expected_min_decision_actions object");
        } else {
            check_min_count(summary, summary.decision_actions, summary.expected_min_decision_actions, "decision action");
        }
    }

    if (extract_raw_value(manifest, "expected_min_scheduler_states", raw)) {
        if (!parse_json_count_object(raw, summary.expected_min_scheduler_states)) {
            fail_assertion(summary, "scenario_manifest.json has invalid expected_min_scheduler_states object");
        } else {
            check_min_count(summary, summary.scheduler_states, summary.expected_min_scheduler_states, "scheduler state");
        }
    }

    if (extract_raw_value(manifest, "expected_min_pressure_bands", raw)) {
        if (!parse_json_count_object(raw, summary.expected_min_pressure_bands)) {
            fail_assertion(summary, "scenario_manifest.json has invalid expected_min_pressure_bands object");
        } else {
            check_min_count(summary, summary.pressure_bands, summary.expected_min_pressure_bands, "pressure band");
        }
    }

    if (extract_raw_value(manifest, "expected_min_risk_bands", raw)) {
        if (!parse_json_count_object(raw, summary.expected_min_risk_bands)) {
            fail_assertion(summary, "scenario_manifest.json has invalid expected_min_risk_bands object");
        } else {
            check_min_count(summary, summary.risk_bands, summary.expected_min_risk_bands, "risk band");
        }
    }
}

void ReplaySummaryBuilder::read_samples(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "samples.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.samples;
        observe_peak(line, "mem_full_avg10", summary.peak_mem_full_avg10);
        observe_peak(line, "gpu_util_pct", summary.peak_gpu_util_pct);
        observe_peak(line, "vram_used_mb", summary.peak_vram_used_mb);

        double vram_free_mb = 0.0;
        if (extract_double(line, "vram_free_mb", vram_free_mb)) {
            if (!summary.saw_vram_free || vram_free_mb < summary.min_vram_free_mb) {
                summary.min_vram_free_mb = vram_free_mb;
            }
            summary.saw_vram_free = true;
        }
    });
}

void ReplaySummaryBuilder::read_processes(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "processes.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.processes;
        increment_if_found(line, "workload_class", summary.process_classes);
    });
}

void ReplaySummaryBuilder::read_scores(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "scores.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.scores;
        observe_peak(line, "ups", summary.peak_ups);
        increment_if_found(line, "band", summary.pressure_bands);
    });
}

void ReplaySummaryBuilder::read_predictions(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "predictions.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.predictions;
        observe_peak(line, "risk_score", summary.peak_risk_score);
        increment_if_found(line, "risk_band", summary.risk_bands);
    });
}

void ReplaySummaryBuilder::read_decisions(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "decisions.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.decisions;
        std::string action;
        std::string level;
        std::string state;

        if (extract_string(line, "action", action) && !action.empty()) {
            ++summary.decision_actions[action];
            observe_signal(summary, "action_" + action);
        }
        if (extract_string(line, "level", level) && !level.empty()) {
            observe_signal(summary, level);
        }
        if (extract_string(line, "state", state) && !state.empty()) {
            ++summary.scheduler_states[state];
            observe_signal(summary, state);
        }
        if (!level.empty() && !action.empty()) {
            observe_signal(summary, level + "_" + action);
        }
    });
}

void ReplaySummaryBuilder::read_actions(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "actions.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.actions;
        increment_if_found(line, "action", summary.action_results);
    });
}

void ReplaySummaryBuilder::read_events(const std::filesystem::path& run_directory, ReplaySummary& summary) const {
    read_ndjson_file(run_directory, "events.ndjson", summary, [&](const std::string& line, std::size_t) {
        ++summary.counts.events;
        increment_if_found(line, "kind", summary.event_kinds);
    });
}

bool ReplaySummaryBuilder::write_summary(
    const ReplaySummary& summary,
    const std::filesystem::path& output_path,
    std::string& error) const {
    try {
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
    } catch (const std::exception& ex) {
        error = std::string("failed to create summary directory: ") + ex.what();
        return false;
    }

    std::ofstream output(output_path);
    if (!output.is_open()) {
        error = "failed to open summary output: " + output_path.string();
        return false;
    }

    output << replay_summary_json(summary) << '\n';
    return true;
}

bool ReplaySummaryBuilder::write_summary_csv(
    const ReplaySummary& summary,
    const std::filesystem::path& output_path,
    std::string& error) const {
    try {
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
    } catch (const std::exception& ex) {
        error = std::string("failed to create summary CSV directory: ") + ex.what();
        return false;
    }

    std::ofstream output(output_path);
    if (!output.is_open()) {
        error = "failed to open summary CSV output: " + output_path.string();
        return false;
    }

    output << replay_summary_csv_header() << '\n'
           << replay_summary_csv_row(summary) << '\n';
    return true;
}

std::string replay_summary_json(const ReplaySummary& summary) {
    const uint64_t duration_wall_ms =
        summary.last_ts_wall >= summary.first_ts_wall ? summary.last_ts_wall - summary.first_ts_wall : 0;
    const uint64_t duration_mono_ms =
        summary.last_ts_mono >= summary.first_ts_mono ? summary.last_ts_mono - summary.first_ts_mono : 0;
    std::map<std::string, double> expected_minimums;
    if (summary.has_expected_min_peak_ups) {
        expected_minimums["peak_ups"] = summary.expected_min_peak_ups;
    }
    if (summary.has_expected_min_peak_risk_score) {
        expected_minimums["peak_risk_score"] = summary.expected_min_peak_risk_score;
    }

    std::ostringstream oss;
    oss << "{\n"
        << "  \"run_id\": \"" << json_escape(summary.run_id) << "\",\n"
        << "  \"scenario\": \"" << json_escape(summary.scenario) << "\",\n"
        << "  \"config_hash\": \"" << json_escape(summary.config_hash) << "\",\n"
        << "  \"valid\": " << (summary.valid ? "true" : "false") << ",\n"
        << "  \"counts\": {\n"
        << "    \"samples\": " << summary.counts.samples << ",\n"
        << "    \"processes\": " << summary.counts.processes << ",\n"
        << "    \"scores\": " << summary.counts.scores << ",\n"
        << "    \"predictions\": " << summary.counts.predictions << ",\n"
        << "    \"decisions\": " << summary.counts.decisions << ",\n"
        << "    \"actions\": " << summary.counts.actions << ",\n"
        << "    \"events\": " << summary.counts.events << ",\n"
        << "    \"parse_errors\": " << summary.counts.parse_errors << "\n"
        << "  },\n"
        << "  \"window\": {\n"
        << "    \"first_ts_wall\": " << summary.first_ts_wall << ",\n"
        << "    \"last_ts_wall\": " << summary.last_ts_wall << ",\n"
        << "    \"duration_wall_ms\": " << duration_wall_ms << ",\n"
        << "    \"first_ts_mono\": " << summary.first_ts_mono << ",\n"
        << "    \"last_ts_mono\": " << summary.last_ts_mono << ",\n"
        << "    \"duration_mono_ms\": " << duration_mono_ms << "\n"
        << "  },\n"
        << "  \"peaks\": {\n"
        << "    \"ups\": " << summary.peak_ups << ",\n"
        << "    \"risk_score\": " << summary.peak_risk_score << ",\n"
        << "    \"mem_full_avg10\": " << summary.peak_mem_full_avg10 << ",\n"
        << "    \"gpu_util_pct\": " << summary.peak_gpu_util_pct << ",\n"
        << "    \"vram_used_mb\": " << summary.peak_vram_used_mb << "\n"
        << "  },\n"
        << "  \"minimums\": {\n"
        << "    \"vram_free_mb\": " << (summary.saw_vram_free ? summary.min_vram_free_mb : 0.0) << "\n"
        << "  },\n"
        << "  \"artifacts\": {\n"
        << "    \"run_metadata_present\": " << (summary.run_metadata_present ? "true" : "false") << ",\n"
        << "    \"run_metadata_bytes\": " << summary.run_metadata_bytes << ",\n"
        << "    \"config_snapshot_present\": " << (summary.config_snapshot_present ? "true" : "false") << ",\n"
        << "    \"config_snapshot_bytes\": " << summary.config_snapshot_bytes << ",\n"
        << "    \"telemetry_quality_present\": " << (summary.telemetry_quality_present ? "true" : "false") << ",\n"
        << "    \"telemetry_quality_bytes\": " << summary.telemetry_quality_bytes << ",\n"
        << "    \"scenario_manifest_present\": " << (summary.scenario_manifest_present ? "true" : "false") << ",\n"
        << "    \"scenario_manifest_bytes\": " << summary.scenario_manifest_bytes << "\n"
        << "  },\n"
        << "  \"pressure_bands\": " << map_json(summary.pressure_bands) << ",\n"
        << "  \"risk_bands\": " << map_json(summary.risk_bands) << ",\n"
        << "  \"scheduler_states\": " << map_json(summary.scheduler_states) << ",\n"
        << "  \"process_classes\": " << map_json(summary.process_classes) << ",\n"
        << "  \"decision_actions\": " << map_json(summary.decision_actions) << ",\n"
        << "  \"action_results\": " << map_json(summary.action_results) << ",\n"
        << "  \"event_kinds\": " << map_json(summary.event_kinds) << ",\n"
        << "  \"manifest_assertions\": {\n"
        << "    \"checked\": " << summary.assertions_checked << ",\n"
        << "    \"passed\": " << summary.assertions_passed << ",\n"
        << "    \"failed\": " << summary.assertions_failed << ",\n"
        << "    \"expected_signals\": " << string_vector_json(summary.expected_signals) << ",\n"
        << "    \"expected_minimums\": " << double_map_json(expected_minimums) << ",\n"
        << "    \"expected_min_decision_actions\": " << map_json(summary.expected_min_decision_actions) << ",\n"
        << "    \"expected_min_scheduler_states\": " << map_json(summary.expected_min_scheduler_states) << ",\n"
        << "    \"expected_min_pressure_bands\": " << map_json(summary.expected_min_pressure_bands) << ",\n"
        << "    \"expected_min_risk_bands\": " << map_json(summary.expected_min_risk_bands) << ",\n"
        << "    \"observed_signals\": " << map_json(summary.observed_signals) << ",\n"
        << "    \"failures\": " << string_vector_json(summary.assertion_failures) << "\n"
        << "  },\n"
        << "  \"warnings\": " << warnings_json(summary.warnings) << "\n"
        << "}";
    return oss.str();
}

std::string replay_summary_csv_header() {
    return "run_id,scenario,config_hash,valid,samples,processes,scores,predictions,decisions,actions,events,"
           "parse_errors,duration_wall_ms,duration_mono_ms,peak_ups,peak_risk_score,peak_mem_full_avg10,"
           "peak_gpu_util_pct,peak_vram_used_mb,min_vram_free_mb,pressure_normal,pressure_elevated,"
           "pressure_critical,risk_low,risk_medium,risk_high,risk_critical,state_normal,state_elevated,"
           "state_throttled,state_recovery,state_cooldown,action_observe,action_reprioritize,action_throttle,"
           "action_resume,action_terminate_candidate,run_metadata_present,config_snapshot_present,"
           "telemetry_quality_present,scenario_manifest_present,assertions_checked,assertions_passed,"
           "assertions_failed,warning_count,assertion_failure_count";
}

std::string replay_summary_csv_row(const ReplaySummary& summary) {
    const uint64_t duration_wall_ms =
        summary.last_ts_wall >= summary.first_ts_wall ? summary.last_ts_wall - summary.first_ts_wall : 0;
    const uint64_t duration_mono_ms =
        summary.last_ts_mono >= summary.first_ts_mono ? summary.last_ts_mono - summary.first_ts_mono : 0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << csv_escape(summary.run_id) << ","
        << csv_escape(summary.scenario) << ","
        << csv_escape(summary.config_hash) << ","
        << bool_csv(summary.valid) << ","
        << summary.counts.samples << ","
        << summary.counts.processes << ","
        << summary.counts.scores << ","
        << summary.counts.predictions << ","
        << summary.counts.decisions << ","
        << summary.counts.actions << ","
        << summary.counts.events << ","
        << summary.counts.parse_errors << ","
        << duration_wall_ms << ","
        << duration_mono_ms << ","
        << summary.peak_ups << ","
        << summary.peak_risk_score << ","
        << summary.peak_mem_full_avg10 << ","
        << summary.peak_gpu_util_pct << ","
        << summary.peak_vram_used_mb << ","
        << (summary.saw_vram_free ? summary.min_vram_free_mb : 0.0) << ","
        << count_for(summary.pressure_bands, "normal") << ","
        << count_for(summary.pressure_bands, "elevated") << ","
        << count_for(summary.pressure_bands, "critical") << ","
        << count_for(summary.risk_bands, "low") << ","
        << count_for(summary.risk_bands, "medium") << ","
        << count_for(summary.risk_bands, "high") << ","
        << count_for(summary.risk_bands, "critical") << ","
        << count_for(summary.scheduler_states, "normal") << ","
        << count_for(summary.scheduler_states, "elevated") << ","
        << count_for(summary.scheduler_states, "throttled") << ","
        << count_for(summary.scheduler_states, "recovery") << ","
        << count_for(summary.scheduler_states, "cooldown") << ","
        << count_for(summary.decision_actions, "observe") << ","
        << count_for(summary.decision_actions, "reprioritize") << ","
        << count_for(summary.decision_actions, "throttle") << ","
        << count_for(summary.decision_actions, "resume") << ","
        << count_for(summary.decision_actions, "terminate_candidate") << ","
        << bool_csv(summary.run_metadata_present) << ","
        << bool_csv(summary.config_snapshot_present) << ","
        << bool_csv(summary.telemetry_quality_present) << ","
        << bool_csv(summary.scenario_manifest_present) << ","
        << summary.assertions_checked << ","
        << summary.assertions_passed << ","
        << summary.assertions_failed << ","
        << summary.warnings.size() << ","
        << summary.assertion_failures.size();
    return oss.str();
}

} // namespace hermes
