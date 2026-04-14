#pragma once

#include <string>
#include <vector>

namespace hermes {

// BenchmarkWorkload describes one workload in a scenario mix.
struct BenchmarkWorkload {
    std::string name;           // human-readable label (e.g. "ml_train_a")
    std::string command;        // shell command to launch
    bool foreground{false};     // if true, Hermes will protect this workload
    bool background{false};     // if true, eligible for reprioritize/throttle/kill
    int duration_s{60};         // how long to let this workload run before killing it
};

// BenchmarkScenario describes a single reproducible test scenario.
// Each scenario should produce one run directory under artifacts/logs/<run_id>/.
struct BenchmarkScenario {
    std::string name;               // scenario identifier (used as HERMES_SCENARIO)
    std::string runtime_mode;       // "observe-only", "advisory", or "active-control"
    int warmup_s{10};               // seconds to wait before starting measurement
    int measurement_s{60};          // total measurement window
    int repeat_count{5};            // how many times to run this scenario
    double ups_elevated_threshold{40.0};
    double ups_critical_threshold{70.0};
    std::vector<BenchmarkWorkload> workloads;

    // Assertion targets (optional; used by replay manifest validation)
    double expected_max_oom_count{0.0};
    double expected_max_p95_latency_ms{0.0};   // 0 = no assertion
    double expected_min_job_completion_rate{0.0}; // 0 = no assertion
};

// ScenarioConfigLoader reads a YAML-like scenario config file.
// The format is intentionally simple (key: value per line, indented workloads).
// Full YAML parsing is not used in v1 to avoid adding a library dependency.
class ScenarioConfigLoader {
public:
    // Load from a file path. Returns true on success.
    // On failure, populates error_out and returns false.
    bool load(const std::string& path, BenchmarkScenario& scenario_out, std::string& error_out);

    // Write a scenario back to a YAML-like file.
    bool save(const std::string& path, const BenchmarkScenario& scenario, std::string& error_out);

    // Build a default baseline scenario with a standard workload mix.
    static BenchmarkScenario default_baseline();

    // Build a default active-control scenario.
    static BenchmarkScenario default_active();
};

} // namespace hermes
