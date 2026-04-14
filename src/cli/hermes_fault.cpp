// hermes_fault: Fault injection fixture generator for Hermes.
//
// Generates labeled samples.ndjson files for specific pressure fault scenarios.
// Output files match exactly the format written by hermesd/hermesd_mt so they
// can be fed directly into hermes_reeval, hermes_eval, and hermes_replay.
//
// Each scenario is written into a self-contained directory under:
//   <out-dir>/<scenario-name>/samples.ndjson
//   <out-dir>/<scenario-name>/scenario_manifest.json
//
// Scenarios:
//   vram_spike       — VRAM ramps from 35% to 98% of 24 GB over 60 samples
//   mem_storm        — mem_full_avg10 spikes 0→50; vmstat_pgmajfault grows fast
//   cpu_hog          — cpu_some_avg10 = 95-99% for 40 samples
//   io_storm         — io_full_avg10 spikes to 35, io_some_avg10 to 50
//   mixed_pressure   — moderate CPU + mem + IO all elevated simultaneously
//   oom_imminent     — VRAM 99%+ AND mem_full_avg10 50+ AND cpu_full_avg10 90+
//   all              — generate all six scenarios
//
// Usage:
//   hermes_fault [--out-dir artifacts/fault_injection] [--scenario <name>]

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- CLI helpers ----

bool has_arg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

std::string get_arg(int argc, char** argv, const std::string& flag, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == flag) return argv[i + 1];
    }
    return def;
}

void print_usage() {
    std::cout
        << "Usage: hermes_fault [--out-dir <dir>] [--scenario <name>]\n"
        << "\n"
        << "Generates labeled fault injection samples.ndjson fixtures.\n"
        << "\n"
        << "Options:\n"
        << "  --out-dir <dir>      Output root (default: artifacts/fault_injection)\n"
        << "  --scenario <name>    One of: vram_spike, mem_storm, cpu_hog, io_storm,\n"
        << "                               mixed_pressure, oom_imminent, all\n"
        << "                       Default: all\n";
}

// ---- Sample record builder ----

struct FaultSample {
    uint64_t ts_wall{0};
    uint64_t ts_mono{0};
    double cpu_some_avg10{0.0};
    double cpu_full_avg10{0.0};
    double mem_some_avg10{0.0};
    double mem_full_avg10{0.0};
    double io_some_avg10{0.0};
    double io_full_avg10{0.0};
    double gpu_util_pct{0.0};
    double vram_used_mb{0.0};
    double vram_total_mb{24576.0}; // 24 GB default
    double vram_free_mb{0.0};
    uint32_t loadavg_runnable{2};
    uint64_t vmstat_pgmajfault{1000};
    uint64_t vmstat_pgfault{50000};
};

std::string sample_to_ndjson(
    const FaultSample& s,
    const std::string& run_id,
    const std::string& scenario) {

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"run_id\":\"" << run_id << "\""
        << ",\"scenario\":\"" << scenario << "\""
        << ",\"config_hash\":\"fault-fixture\""
        << ",\"ts_wall\":" << s.ts_wall
        << ",\"ts_mono\":" << s.ts_mono
        << ",\"cpu_some_avg10\":" << s.cpu_some_avg10
        << ",\"cpu_full_avg10\":" << s.cpu_full_avg10
        << ",\"mem_some_avg10\":" << s.mem_some_avg10
        << ",\"mem_full_avg10\":" << s.mem_full_avg10
        << ",\"gpu_util_pct\":" << s.gpu_util_pct
        << ",\"vram_used_mb\":" << s.vram_used_mb
        << ",\"vram_total_mb\":" << s.vram_total_mb
        << ",\"vram_free_mb\":" << s.vram_free_mb
        << ",\"io_some_avg10\":" << s.io_some_avg10
        << ",\"io_full_avg10\":" << s.io_full_avg10
        << ",\"vmstat_pgmajfault\":" << s.vmstat_pgmajfault
        << ",\"vmstat_pgfault\":" << s.vmstat_pgfault
        << ",\"loadavg_runnable\":" << s.loadavg_runnable
        << ",\"cpu_available\":true"
        << ",\"mem_available\":true"
        << ",\"loadavg_available\":true"
        << ",\"gpu_available\":true"
        << ",\"io_available\":true"
        << ",\"vmstat_available\":true"
        << "}";
    return oss.str();
}

bool write_scenario(
    const std::filesystem::path& out_dir,
    const std::string& scenario_name,
    const std::vector<FaultSample>& samples,
    const std::string& description) {

    const std::filesystem::path dir = out_dir / scenario_name;
    try {
        std::filesystem::create_directories(dir);
    } catch (const std::exception& ex) {
        std::cerr << "hermes_fault: failed to create " << dir << ": " << ex.what() << "\n";
        return false;
    }

    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string run_id = "fault-" + scenario_name + "-" + std::to_string(now_ms);

    // Write samples.ndjson
    const std::filesystem::path samples_path = dir / "samples.ndjson";
    {
        std::ofstream f(samples_path);
        if (!f.is_open()) {
            std::cerr << "hermes_fault: cannot write " << samples_path << "\n";
            return false;
        }
        for (const FaultSample& s : samples) {
            f << sample_to_ndjson(s, run_id, scenario_name) << "\n";
        }
    }

    // Write scenario_manifest.json
    const std::filesystem::path manifest_path = dir / "scenario_manifest.json";
    {
        std::ofstream f(manifest_path);
        if (!f.is_open()) {
            std::cerr << "hermes_fault: cannot write " << manifest_path << "\n";
            return false;
        }
        f << "{\n"
          << "  \"run_id\": \"" << run_id << "\",\n"
          << "  \"scenario\": \"" << scenario_name << "\",\n"
          << "  \"description\": \"" << description << "\",\n"
          << "  \"sample_count\": " << samples.size() << ",\n"
          << "  \"cadence_ms\": 500,\n"
          << "  \"fault_injection\": true\n"
          << "}\n";
    }

    std::cout << "  [" << scenario_name << "] "
              << samples.size() << " samples -> " << dir.string() << "\n";
    return true;
}

// ---- Scenario generators ----

// Baseline: 30 warm-up samples at low pressure, then 60 fault samples
static constexpr uint64_t BASE_WALL_MS = 1700000000000ULL; // fixed epoch anchor
static constexpr uint64_t BASE_MONO_MS = 1000ULL;
static constexpr uint64_t STEP_MS      = 500ULL;

std::vector<FaultSample> make_warmup(int count = 20) {
    std::vector<FaultSample> samples;
    for (int i = 0; i < count; ++i) {
        FaultSample s;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(i) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(i) * STEP_MS;
        s.cpu_some_avg10 = 4.0 + 0.5 * i;
        s.cpu_full_avg10 = 0.5;
        s.mem_some_avg10 = 2.0;
        s.mem_full_avg10 = 0.2;
        s.gpu_util_pct   = 45.0;
        s.vram_used_mb   = 8000.0;
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - s.vram_used_mb;
        s.loadavg_runnable = 4;
        s.vmstat_pgmajfault = 1000 + static_cast<uint64_t>(i) * 2;
        s.vmstat_pgfault    = 50000 + static_cast<uint64_t>(i) * 100;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 1: VRAM spike — linear ramp from 35% to 98% of 24 GB over 60 fault samples
std::vector<FaultSample> gen_vram_spike() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const double vram_total = 24576.0;
    const double vram_start = vram_total * 0.35;
    const double vram_end   = vram_total * 0.98;
    const int fault_count   = 60;
    for (int i = 0; i < fault_count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(fault_count - 1);
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.cpu_some_avg10 = 8.0 + 2.0 * t;
        s.cpu_full_avg10 = 0.5;
        s.mem_some_avg10 = 3.0 + 5.0 * t;
        s.mem_full_avg10 = 0.3 + 1.5 * t;
        s.gpu_util_pct   = 80.0 + 15.0 * t;
        s.vram_used_mb   = vram_start + (vram_end - vram_start) * t;
        s.vram_total_mb  = vram_total;
        s.vram_free_mb   = vram_total - s.vram_used_mb;
        s.loadavg_runnable = 6 + static_cast<uint32_t>(4.0 * t);
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 3;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 500;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 2: Memory reclaim storm — mem_full spikes + pgmajfault growth + IO pressure
std::vector<FaultSample> gen_mem_storm() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const int fault_count = 50;
    for (int i = 0; i < fault_count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(fault_count - 1);
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.cpu_some_avg10 = 15.0 + 20.0 * t;
        s.cpu_full_avg10 = 5.0  + 15.0 * t;
        s.mem_some_avg10 = 20.0 + 40.0 * t;
        s.mem_full_avg10 = 5.0  + 45.0 * t;
        s.io_some_avg10  = 10.0 + 25.0 * t;
        s.io_full_avg10  = 2.0  + 18.0 * t;
        s.gpu_util_pct   = 50.0;
        s.vram_used_mb   = 9000.0;
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - s.vram_used_mb;
        s.loadavg_runnable = 8 + static_cast<uint32_t>(12.0 * t);
        // Major faults grow aggressively during reclaim storm
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 80;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 2000;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 3: CPU hog — cpu_some_avg10 pinned near 100% for 40 samples
std::vector<FaultSample> gen_cpu_hog() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const int fault_count = 40;
    for (int i = 0; i < fault_count; ++i) {
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        // Slightly varying to simulate measurement noise
        s.cpu_some_avg10 = 95.0 + 4.0 * std::sin(static_cast<double>(i) * 0.4);
        s.cpu_full_avg10 = 60.0 + 20.0 * std::sin(static_cast<double>(i) * 0.3);
        s.mem_some_avg10 = 5.0;
        s.mem_full_avg10 = 0.5;
        s.gpu_util_pct   = 30.0;
        s.vram_used_mb   = 7000.0;
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - s.vram_used_mb;
        s.loadavg_runnable = 24;
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 2;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 300;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 4: IO storm — io_full spikes to 35; IO waits dominate
std::vector<FaultSample> gen_io_storm() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const int fault_count = 45;
    for (int i = 0; i < fault_count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(fault_count - 1);
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.cpu_some_avg10 = 8.0;
        s.cpu_full_avg10 = 1.0;
        s.mem_some_avg10 = 5.0;
        s.mem_full_avg10 = 0.5;
        // IO ramps up sharply, then plateaus
        const double io_ramp = (t < 0.5) ? t * 2.0 : 1.0;
        s.io_some_avg10  = 50.0 * io_ramp;
        s.io_full_avg10  = 35.0 * io_ramp;
        s.gpu_util_pct   = 60.0;
        s.vram_used_mb   = 10000.0;
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - s.vram_used_mb;
        s.loadavg_runnable = 6;
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 15;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 800;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 5: Mixed pressure — CPU + mem + IO all moderately elevated
std::vector<FaultSample> gen_mixed_pressure() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const int fault_count = 50;
    for (int i = 0; i < fault_count; ++i) {
        const double noise = std::sin(static_cast<double>(i) * 0.7);
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.cpu_some_avg10 = 45.0 + 8.0 * noise;
        s.cpu_full_avg10 = 12.0 + 3.0 * noise;
        s.mem_some_avg10 = 35.0 + 6.0 * noise;
        s.mem_full_avg10 = 8.0  + 2.0 * noise;
        s.io_some_avg10  = 18.0 + 4.0 * noise;
        s.io_full_avg10  = 6.0  + 1.5 * noise;
        s.gpu_util_pct   = 70.0 + 10.0 * noise;
        s.vram_used_mb   = 16000.0 + 500.0 * noise;
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - s.vram_used_mb;
        s.loadavg_runnable = 12;
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 10;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 600;
        samples.push_back(s);
    }
    return samples;
}

// Scenario 6: OOM imminent — VRAM 99%+ AND mem_full 50+ AND cpu_full 90+
std::vector<FaultSample> gen_oom_imminent() {
    auto samples = make_warmup();
    const int warmup = static_cast<int>(samples.size());
    const int fault_count = 30;
    for (int i = 0; i < fault_count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(fault_count - 1);
        FaultSample s;
        const int idx = warmup + i;
        s.ts_wall = BASE_WALL_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.ts_mono = BASE_MONO_MS + static_cast<uint64_t>(idx) * STEP_MS;
        s.cpu_some_avg10 = 80.0 + 15.0 * t;
        s.cpu_full_avg10 = 50.0 + 40.0 * t;
        s.mem_some_avg10 = 60.0 + 30.0 * t;
        s.mem_full_avg10 = 30.0 + 50.0 * t;
        s.io_some_avg10  = 20.0 + 10.0 * t;
        s.io_full_avg10  = 8.0  + 6.0  * t;
        s.gpu_util_pct   = 95.0 + 4.5 * t;
        s.vram_used_mb   = 24000.0 + 500.0 * t; // exceeds 24576 MB near end
        s.vram_total_mb  = 24576.0;
        s.vram_free_mb   = s.vram_total_mb - std::min(s.vram_used_mb, s.vram_total_mb);
        s.loadavg_runnable = 16 + static_cast<uint32_t>(8.0 * t);
        // Extremely fast major fault growth
        s.vmstat_pgmajfault = 1040 + static_cast<uint64_t>(i) * 250;
        s.vmstat_pgfault    = 52000 + static_cast<uint64_t>(i) * 5000;
        samples.push_back(s);
    }
    return samples;
}

} // namespace

int main(int argc, char** argv) {
    if (has_arg(argc, argv, "--help") || has_arg(argc, argv, "-h")) {
        print_usage();
        return 0;
    }

    const std::string out_dir_str = get_arg(argc, argv, "--out-dir",
        std::getenv("HERMES_ARTIFACT_ROOT") != nullptr
            ? std::string(std::getenv("HERMES_ARTIFACT_ROOT")) + "/fault_injection"
            : "artifacts/fault_injection");
    const std::string scenario = get_arg(argc, argv, "--scenario", "all");

    const std::filesystem::path out_dir = out_dir_str;

    std::cout << "hermes_fault: generating fault injection fixtures\n";
    std::cout << "Output dir : " << out_dir.string() << "\n";
    std::cout << "Scenario   : " << scenario << "\n\n";

    struct ScenarioDef {
        std::string name;
        std::string description;
        std::vector<FaultSample> (*gen)();
    };

    const std::vector<ScenarioDef> all_scenarios = {
        {"vram_spike",      "VRAM ramps from 35% to 98% of 24 GB over 60 samples",               gen_vram_spike},
        {"mem_storm",       "mem_full_avg10 spikes 0->50; vmstat_pgmajfault grows rapidly",       gen_mem_storm},
        {"cpu_hog",         "cpu_some_avg10 pinned at 95-99% for 40 samples",                    gen_cpu_hog},
        {"io_storm",        "io_full_avg10 spikes to 35, io_some_avg10 to 50 over 45 samples",   gen_io_storm},
        {"mixed_pressure",  "CPU + mem + IO all moderately elevated simultaneously",              gen_mixed_pressure},
        {"oom_imminent",    "VRAM 99%+ AND mem_full 50+ AND cpu_full 90+ — imminent OOM",        gen_oom_imminent},
    };

    int written = 0;
    int failed  = 0;

    for (const ScenarioDef& def : all_scenarios) {
        if (scenario != "all" && scenario != def.name) continue;
        const std::vector<FaultSample> samples = def.gen();
        if (write_scenario(out_dir, def.name, samples, def.description)) {
            ++written;
        } else {
            ++failed;
        }
    }

    if (scenario != "all") {
        bool found = false;
        for (const ScenarioDef& def : all_scenarios) {
            if (def.name == scenario) { found = true; break; }
        }
        if (!found) {
            std::cerr << "hermes_fault: unknown scenario '" << scenario << "'\n";
            print_usage();
            return 1;
        }
    }

    std::cout << "\nDone. " << written << " scenario(s) written";
    if (failed > 0) std::cout << ", " << failed << " failed";
    std::cout << ".\n";
    return failed > 0 ? 1 : 0;
}
