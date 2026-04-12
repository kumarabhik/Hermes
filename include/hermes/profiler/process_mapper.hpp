#pragma once

#include "hermes/core/types.hpp"
#include "hermes/profiler/proc_stat.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace hermes {

class ProcessMapper {
public:
    ProcessMapper();

    std::vector<ProcessSnapshot> collect(const std::vector<GpuProcessUsage>& gpu_processes);

private:
    struct CpuHistory {
        uint64_t total_cpu_ticks{0};
        std::chrono::steady_clock::time_point sampled_at{};
    };

    double compute_cpu_pct(int pid, uint64_t total_cpu_ticks, std::chrono::steady_clock::time_point now);

    ProcStatReader proc_stat_reader_;
    std::unordered_map<int, CpuHistory> cpu_history_;
    long clock_ticks_per_second_{100};
    long page_size_bytes_{4096};
};

} // namespace hermes
