#include "hermes/profiler/process_mapper.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace hermes {
namespace {

bool is_numeric_directory(const std::string& name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

} // namespace

ProcessMapper::ProcessMapper() {
#if !defined(_WIN32)
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks > 0) {
        clock_ticks_per_second_ = ticks;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        page_size_bytes_ = page_size;
    }
#endif
}

double ProcessMapper::compute_cpu_pct(int pid, uint64_t total_cpu_ticks, std::chrono::steady_clock::time_point now) {
    const auto it = cpu_history_.find(pid);
    if (it == cpu_history_.end()) {
        cpu_history_[pid] = CpuHistory{total_cpu_ticks, now};
        return 0.0;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.sampled_at).count();
    if (elapsed <= 0 || total_cpu_ticks < it->second.total_cpu_ticks) {
        it->second = CpuHistory{total_cpu_ticks, now};
        return 0.0;
    }

    const uint64_t delta_ticks = total_cpu_ticks - it->second.total_cpu_ticks;
    it->second = CpuHistory{total_cpu_ticks, now};

    const double cpu_seconds = static_cast<double>(delta_ticks) / static_cast<double>(clock_ticks_per_second_);
    const double wall_seconds = static_cast<double>(elapsed) / 1000.0;
    return wall_seconds > 0.0 ? (cpu_seconds / wall_seconds) * 100.0 : 0.0;
}

std::vector<ProcessSnapshot> ProcessMapper::collect(const std::vector<GpuProcessUsage>& gpu_processes) {
    const std::filesystem::path proc_root("/proc");
    if (!std::filesystem::exists(proc_root)) {
        return {};
    }

    std::unordered_map<int, double> gpu_by_pid;
    for (const GpuProcessUsage& usage : gpu_processes) {
        gpu_by_pid[usage.pid] += usage.gpu_mb;
    }

    const auto now = std::chrono::steady_clock::now();
    std::unordered_set<int> seen_pids;
    std::vector<ProcessSnapshot> snapshots;

    for (const auto& entry : std::filesystem::directory_iterator(proc_root)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string dirname = entry.path().filename().string();
        if (!is_numeric_directory(dirname)) {
            continue;
        }

        const int pid = std::stoi(dirname);
        ProcStatRecord stat_record;
        if (!proc_stat_reader_.read(pid, stat_record)) {
            continue;
        }

        ProcessSnapshot snapshot;
        snapshot.pid = stat_record.pid;
        snapshot.ppid = stat_record.ppid;
        snapshot.cmd = stat_record.cmdline.empty() ? stat_record.comm : stat_record.cmdline;
        snapshot.state = std::string(1, stat_record.state);
        snapshot.nice = static_cast<int>(stat_record.nice);
        snapshot.rss_mb = (static_cast<double>(stat_record.rss_pages) * static_cast<double>(page_size_bytes_)) /
            (1024.0 * 1024.0);
        snapshot.gpu_mb = gpu_by_pid.count(stat_record.pid) != 0 ? gpu_by_pid[stat_record.pid] : 0.0;
        snapshot.total_cpu_ticks = stat_record.utime_ticks + stat_record.stime_ticks;
        snapshot.cpu_pct = compute_cpu_pct(stat_record.pid, snapshot.total_cpu_ticks, now);

        seen_pids.insert(snapshot.pid);
        snapshots.push_back(snapshot);
    }

    for (auto it = cpu_history_.begin(); it != cpu_history_.end();) {
        if (seen_pids.count(it->first) == 0) {
            it = cpu_history_.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const ProcessSnapshot& left, const ProcessSnapshot& right) {
        if (left.gpu_mb != right.gpu_mb) {
            return left.gpu_mb > right.gpu_mb;
        }
        return left.cpu_pct > right.cpu_pct;
    });

    return snapshots;
}

} // namespace hermes
