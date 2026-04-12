#include "hermes/runtime/run_metadata.hpp"

#include "hermes/runtime/event_logger.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? "" : value;
}

std::string host_name() {
    std::string value = env_or_empty("HOSTNAME");
    if (!value.empty()) {
        return value;
    }
    value = env_or_empty("COMPUTERNAME");
    return value.empty() ? "unknown" : value;
}

std::string os_family() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "unknown";
#endif
}

std::string compiler_description() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

int process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

std::string path_string(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool write_text_file(const std::filesystem::path& path, const std::string& content, std::string& error) {
    std::ofstream output(path, std::ios::out | std::ios::binary);
    if (!output.is_open()) {
        error = "failed to open output file: " + path.string();
        return false;
    }

    output << content;
    return true;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

} // namespace

bool RunMetadataWriter::write(const RunMetadataConfig& config, std::string& error) const {
    if (config.run_directory.empty()) {
        error = "run directory is required for metadata writing";
        return false;
    }

    try {
        std::filesystem::create_directories(config.run_directory);
    } catch (const std::exception& ex) {
        error = std::string("failed to create run directory for metadata: ") + ex.what();
        return false;
    }

    return write_config_snapshot(config, error) && write_run_metadata(config, error);
}

bool RunMetadataWriter::write_config_snapshot(const RunMetadataConfig& config, std::string& error) const {
    const std::filesystem::path output_path = config.run_directory / "config_snapshot.yaml";
    const std::string config_text = read_text_file(config.config_path);

    if (config_text.empty()) {
        std::ostringstream fallback;
        fallback << "# Hermes config snapshot unavailable\n"
                 << "# source: " << path_string(config.config_path) << "\n";
        return write_text_file(output_path, fallback.str(), error);
    }

    return write_text_file(output_path, config_text, error);
}

bool RunMetadataWriter::write_run_metadata(const RunMetadataConfig& config, std::string& error) const {
    const uint64_t ts_wall = config.started_ts_wall == 0 ? wall_now_ms() : config.started_ts_wall;
    const uint64_t ts_mono = config.started_ts_mono == 0 ? mono_now_ms() : config.started_ts_mono;
    const std::filesystem::path metadata_path = config.run_directory / "run_metadata.json";
    const std::filesystem::path snapshot_path = config.run_directory / "config_snapshot.yaml";

    const bool proc_available = std::filesystem::exists("/proc");
    const bool cpu_psi_available = std::filesystem::exists("/proc/pressure/cpu");
    const bool mem_psi_available = std::filesystem::exists("/proc/pressure/memory");
    const bool loadavg_available = std::filesystem::exists("/proc/loadavg");
    const bool config_source_available = std::filesystem::exists(config.config_path);
    const bool config_snapshot_available = std::filesystem::exists(snapshot_path);

    std::ostringstream json;
    json << "{\n"
         << "  \"run_id\": \"" << json_escape(config.run_id) << "\",\n"
         << "  \"scenario\": \"" << json_escape(config.scenario) << "\",\n"
         << "  \"config_hash\": \"" << json_escape(config.config_hash) << "\",\n"
         << "  \"runtime_mode\": \"" << json_escape(config.runtime_mode) << "\",\n"
         << "  \"started_ts_wall\": " << ts_wall << ",\n"
         << "  \"started_ts_mono\": " << ts_mono << ",\n"
         << "  \"artifact_root\": \"" << json_escape(path_string(config.artifact_root)) << "\",\n"
         << "  \"run_directory\": \"" << json_escape(path_string(config.run_directory)) << "\",\n"
         << "  \"config_source\": \"" << json_escape(path_string(config.config_path)) << "\",\n"
         << "  \"config_snapshot\": \"" << json_escape(path_string(snapshot_path)) << "\",\n"
         << "  \"host\": {\n"
         << "    \"hostname\": \"" << json_escape(host_name()) << "\",\n"
         << "    \"os_family\": \"" << os_family() << "\",\n"
         << "    \"working_directory\": \"" << json_escape(path_string(std::filesystem::current_path())) << "\",\n"
         << "    \"process_id\": " << process_id() << "\n"
         << "  },\n"
         << "  \"build\": {\n"
         << "    \"compiler\": \"" << json_escape(compiler_description()) << "\",\n"
         << "    \"cplusplus\": " << __cplusplus << "\n"
         << "  },\n"
         << "  \"feature_probes\": {\n"
         << "    \"proc_available\": " << bool_literal(proc_available) << ",\n"
         << "    \"cpu_psi_available\": " << bool_literal(cpu_psi_available) << ",\n"
         << "    \"mem_psi_available\": " << bool_literal(mem_psi_available) << ",\n"
         << "    \"loadavg_available\": " << bool_literal(loadavg_available) << ",\n"
         << "    \"config_source_available\": " << bool_literal(config_source_available) << ",\n"
         << "    \"config_snapshot_available\": " << bool_literal(config_snapshot_available) << "\n"
         << "  }\n"
         << "}\n";

    return write_text_file(metadata_path, json.str(), error);
}

} // namespace hermes
