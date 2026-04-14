#include "hermes/runtime/event_logger.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hermes {
namespace {

uint64_t wall_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t mono_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* bool_literal(bool value) {
    return value ? "true" : "false";
}

} // namespace

std::string json_escape(const std::string& value) {
    std::ostringstream oss;
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\b':
            oss << "\\b";
            break;
        case '\f':
            oss << "\\f";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                oss << ch;
            }
            break;
        }
    }
    return oss.str();
}

std::string json_string_array(const std::vector<std::string>& values) {
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

std::string json_int_array(const std::vector<int>& values) {
    std::ostringstream oss;
    oss << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << values[index];
    }
    oss << "]";
    return oss.str();
}

EventLogger::EventLogger(EventLoggerConfig config)
    : config_(std::move(config)) {}

bool EventLogger::open() {
    if (!config_.enabled) {
        return true;
    }

    if (config_.run_id.empty()) {
        last_error_ = "run_id is required for event logging";
        return false;
    }

    run_directory_ = config_.artifact_root / "logs" / config_.run_id;

    try {
        std::filesystem::create_directories(run_directory_);
    } catch (const std::exception& ex) {
        last_error_ = std::string("failed to create run artifact directory: ") + ex.what();
        return false;
    }

    opened_ =
        open_stream(samples_, "samples.ndjson") &&
        open_stream(processes_, "processes.ndjson") &&
        open_stream(scores_, "scores.ndjson") &&
        open_stream(predictions_, "predictions.ndjson") &&
        open_stream(decisions_, "decisions.ndjson") &&
        open_stream(actions_, "actions.ndjson") &&
        open_stream(events_, "events.ndjson");

    return opened_;
}

bool EventLogger::open_stream(std::ofstream& stream, const std::string& filename) {
    stream.open(run_directory_ / filename, std::ios::out | std::ios::app);
    if (!stream.is_open()) {
        last_error_ = "failed to open artifact file: " + (run_directory_ / filename).string();
        return false;
    }
    return true;
}

void EventLogger::write_line(std::ofstream& stream, const std::string& line) {
    if (!opened_ || !stream.is_open()) {
        return;
    }

    stream << line << '\n';
    stream.flush();
}

std::string EventLogger::common_fields(uint64_t ts_wall, uint64_t ts_mono) const {
    std::ostringstream oss;
    oss << "\"run_id\":\"" << json_escape(config_.run_id) << "\","
        << "\"scenario\":\"" << json_escape(config_.scenario) << "\","
        << "\"config_hash\":\"" << json_escape(config_.config_hash) << "\","
        << "\"ts_wall\":" << ts_wall << ","
        << "\"ts_mono\":" << ts_mono;
    return oss.str();
}

void EventLogger::log_run_start(const std::string& runtime_mode) {
    std::ostringstream payload;
    payload << "{\"runtime_mode\":\"" << json_escape(runtime_mode) << "\","
            << "\"run_directory\":\"" << json_escape(run_directory_.string()) << "\"}";
    log_event("run_start", payload.str());
}

void EventLogger::log_sample(
    const PressureSample& sample,
    bool cpu_available,
    bool mem_available,
    bool loadavg_available,
    bool gpu_available,
    bool io_available,
    bool vmstat_available) {
    std::ostringstream oss;
    oss << "{" << common_fields(sample.ts_wall, sample.ts_mono)
        << ",\"cpu_some_avg10\":" << sample.cpu_some_avg10
        << ",\"cpu_full_avg10\":" << sample.cpu_full_avg10
        << ",\"mem_some_avg10\":" << sample.mem_some_avg10
        << ",\"mem_full_avg10\":" << sample.mem_full_avg10
        << ",\"gpu_util_pct\":" << sample.gpu_util_pct
        << ",\"vram_used_mb\":" << sample.vram_used_mb
        << ",\"vram_total_mb\":" << sample.vram_total_mb
        << ",\"vram_free_mb\":" << sample.vram_free_mb
        << ",\"io_some_avg10\":" << sample.io_some_avg10
        << ",\"io_full_avg10\":" << sample.io_full_avg10
        << ",\"vmstat_pgmajfault\":" << sample.vmstat_pgmajfault
        << ",\"vmstat_pgfault\":" << sample.vmstat_pgfault
        << ",\"loadavg_runnable\":" << sample.loadavg_runnable
        << ",\"cpu_available\":" << bool_literal(cpu_available)
        << ",\"mem_available\":" << bool_literal(mem_available)
        << ",\"loadavg_available\":" << bool_literal(loadavg_available)
        << ",\"gpu_available\":" << bool_literal(gpu_available)
        << ",\"io_available\":" << bool_literal(io_available)
        << ",\"vmstat_available\":" << bool_literal(vmstat_available)
        << "}";
    write_line(samples_, oss.str());
}

void EventLogger::log_processes(const PressureSample& sample, const std::vector<ProcessSnapshot>& processes) {
    for (const ProcessSnapshot& process : processes) {
        std::ostringstream oss;
        oss << "{" << common_fields(sample.ts_wall, sample.ts_mono)
            << ",\"pid\":" << process.pid
            << ",\"ppid\":" << process.ppid
            << ",\"cmd\":\"" << json_escape(process.cmd) << "\""
            << ",\"state\":\"" << json_escape(process.state) << "\""
            << ",\"nice\":" << process.nice
            << ",\"cpu_pct\":" << process.cpu_pct
            << ",\"rss_mb\":" << process.rss_mb
            << ",\"gpu_mb\":" << process.gpu_mb
            << ",\"workload_class\":\"" << to_string(process.workload_class) << "\""
            << ",\"foreground\":" << bool_literal(process.foreground)
            << ",\"protected\":" << bool_literal(process.protected_process)
            << ",\"total_cpu_ticks\":" << process.total_cpu_ticks
            << "}";
        write_line(processes_, oss.str());
    }
}

void EventLogger::log_score(const PressureScore& score) {
    std::ostringstream oss;
    oss << "{" << common_fields(wall_now_ms(), score.ts_mono)
        << ",\"ups\":" << score.ups
        << ",\"band\":\"" << to_string(score.band) << "\""
        << ",\"previous_band\":\"" << to_string(score.previous_band) << "\""
        << ",\"band_changed\":" << bool_literal(score.band_changed)
        << ",\"n_cpu\":" << score.components.n_cpu
        << ",\"n_mem\":" << score.components.n_mem
        << ",\"n_gpu_util\":" << score.components.n_gpu_util
        << ",\"n_vram\":" << score.components.n_vram
        << ",\"n_io\":" << score.components.n_io
        << ",\"weighted_cpu\":" << score.components.weighted_cpu
        << ",\"weighted_mem\":" << score.components.weighted_mem
        << ",\"weighted_gpu_util\":" << score.components.weighted_gpu_util
        << ",\"weighted_vram\":" << score.components.weighted_vram
        << ",\"weighted_io\":" << score.components.weighted_io
        << ",\"dominant_signals\":" << json_string_array(score.dominant_signals)
        << "}";
    write_line(scores_, oss.str());

    if (score.band_changed) {
        std::ostringstream payload;
        payload << "{\"previous_band\":\"" << to_string(score.previous_band) << "\","
                << "\"band\":\"" << to_string(score.band) << "\","
                << "\"ups\":" << score.ups << ","
                << "\"dominant_signals\":" << json_string_array(score.dominant_signals) << "}";
        log_event("band_transition", payload.str());
    }
}

void EventLogger::log_prediction(const RiskPrediction& prediction) {
    std::ostringstream oss;
    oss << "{" << common_fields(wall_now_ms(), prediction.ts_mono)
        << ",\"risk_score\":" << prediction.risk_score
        << ",\"risk_band\":\"" << to_string(prediction.risk_band) << "\""
        << ",\"predicted_event\":\"" << json_escape(prediction.predicted_event) << "\""
        << ",\"lead_time_s\":" << prediction.lead_time_s
        << ",\"reason_codes\":" << json_string_array(prediction.reason_codes)
        << ",\"target_pids\":" << json_int_array(prediction.target_pids)
        << ",\"recommended_action\":\"" << to_string(prediction.recommended_action) << "\""
        << "}";
    write_line(predictions_, oss.str());
}

void EventLogger::log_decision(
    const PressureScore& score,
    const RiskPrediction& prediction,
    const InterventionDecision& decision) {
    std::ostringstream oss;
    oss << "{" << common_fields(wall_now_ms(), decision.ts_mono)
        << ",\"ups\":" << score.ups
        << ",\"pressure_band\":\"" << to_string(score.band) << "\""
        << ",\"risk_score\":" << prediction.risk_score
        << ",\"risk_band\":\"" << to_string(prediction.risk_band) << "\""
        << ",\"predicted_event\":\"" << json_escape(prediction.predicted_event) << "\""
        << ",\"lead_time_s\":" << prediction.lead_time_s
        << ",\"level\":\"" << to_string(decision.level) << "\""
        << ",\"action\":\"" << to_string(decision.action) << "\""
        << ",\"target_pids\":" << json_int_array(decision.target_pids)
        << ",\"previous_state\":\"" << to_string(decision.previous_scheduler_state) << "\""
        << ",\"state\":\"" << to_string(decision.scheduler_state) << "\""
        << ",\"state_changed\":" << bool_literal(decision.scheduler_state_changed)
        << ",\"cooldown_state\":\"" << json_escape(decision.cooldown_state) << "\""
        << ",\"why\":\"" << json_escape(decision.why) << "\""
        << ",\"mode\":\"" << to_string(decision.mode) << "\""
        << ",\"should_execute\":" << bool_literal(decision.should_execute)
        << "}";
    write_line(decisions_, oss.str());

    if (decision.scheduler_state_changed) {
        std::ostringstream payload;
        payload << "{\"previous_state\":\"" << to_string(decision.previous_scheduler_state) << "\","
                << "\"state\":\"" << to_string(decision.scheduler_state) << "\","
                << "\"action\":\"" << to_string(decision.action) << "\","
                << "\"why\":\"" << json_escape(decision.why) << "\"}";
        log_event("state_transition", payload.str());
    }
}

void EventLogger::log_action(const InterventionDecision& decision, const InterventionResult& result) {
    std::ostringstream oss;
    oss << "{" << common_fields(wall_now_ms(), result.ts_mono)
        << ",\"action\":\"" << to_string(decision.action) << "\""
        << ",\"level\":\"" << to_string(decision.level) << "\""
        << ",\"target_pids\":" << json_int_array(decision.target_pids)
        << ",\"success\":" << bool_literal(result.success)
        << ",\"error\":\"" << json_escape(result.error) << "\""
        << ",\"system_effect\":\"" << json_escape(result.system_effect) << "\""
        << ",\"reverted\":" << bool_literal(result.reverted)
        << ",\"reversal_condition\":\"" << json_escape(result.reversal_condition) << "\""
        << ",\"mode\":\"" << to_string(decision.mode) << "\""
        << "}";
    write_line(actions_, oss.str());

    std::ostringstream payload;
    payload << "{\"action\":\"" << to_string(decision.action) << "\","
            << "\"success\":" << bool_literal(result.success) << ","
            << "\"system_effect\":\"" << json_escape(result.system_effect) << "\"}";
    log_event("action_result", payload.str());
}

void EventLogger::log_event(const std::string& kind, const std::string& payload_json) {
    std::ostringstream oss;
    oss << "{" << common_fields(wall_now_ms(), mono_now_ms())
        << ",\"kind\":\"" << json_escape(kind) << "\""
        << ",\"payload\":" << (payload_json.empty() ? "{}" : payload_json)
        << "}";
    write_line(events_, oss.str());
}

} // namespace hermes
