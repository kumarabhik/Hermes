#pragma once

#include "hermes/core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace hermes {

// CgroupV2Backend applies privileged CPU and memory controls using the Linux
// cgroup v2 unified hierarchy. All operations require write access to the
// cgroup filesystem (typically under /sys/fs/cgroup/).
//
// Controls implemented:
//   cpu.max    — CPU bandwidth quota (throttle background work)
//   memory.high — Soft memory ceiling (trigger reclaim before hard OOM)
//   cpuset.cpus — CPU affinity isolation (pin foreground to fast cores)
//
// Safety model:
//   - Every mutation records the previous value before writing.
//   - restore() re-applies the saved previous values.
//   - On non-Linux platforms all operations return a simulated result.
//   - An unavailable cgroup path is treated as a non-fatal degradation.

struct CgroupControl {
    std::string cgroup_path;    // e.g. /sys/fs/cgroup/hermes/bg_jobs
    std::string control_file;   // e.g. cpu.max, memory.high
    std::string value;          // value to write
    std::string previous_value; // value before mutation (for restore)
};

struct CgroupResult {
    bool success{false};
    std::string error;
    std::string cgroup_path;
    std::string control_file;
    std::string previous_value;
    std::string applied_value;
    std::string reversal_condition;
};

class CgroupV2Backend {
public:
    explicit CgroupV2Backend(std::string cgroup_root = "/sys/fs/cgroup/hermes");

    // Check whether the cgroup root exists and is writable.
    bool is_available() const;

    // Apply a CPU bandwidth quota to a cgroup.
    // quota_us: microseconds of CPU time per period_us.
    // e.g. quota_us=50000, period_us=100000 means 50% of one CPU core.
    // Write "max" to remove the limit.
    CgroupResult set_cpu_max(
        const std::string& cgroup_name,
        uint64_t quota_us,
        uint64_t period_us = 100000);

    // Apply a soft memory ceiling (memory.high). Processes above this will be
    // throttled and reclaim pressure increases, but OOM is not immediate.
    CgroupResult set_memory_high(
        const std::string& cgroup_name,
        uint64_t bytes);

    // Pin a cgroup to specific CPUs.
    // cpu_list: cpuset format e.g. "0-3" or "0,2,4"
    CgroupResult set_cpuset(
        const std::string& cgroup_name,
        const std::string& cpu_list);

    // Move a PID into a cgroup. Creates the cgroup if it does not exist.
    CgroupResult attach_pid(
        const std::string& cgroup_name,
        int pid);

    // Restore all cgroups to their pre-mutation values.
    // Returns the number of controls successfully restored.
    int restore_all();

    // Read current value of a control file.
    std::string read_control(
        const std::string& cgroup_name,
        const std::string& control_file) const;

    const std::vector<CgroupControl>& saved_controls() const {
        return saved_controls_;
    }

private:
    std::string cgroup_root_;
    std::vector<CgroupControl> saved_controls_;

    std::string cgroup_path(const std::string& cgroup_name) const;
    bool ensure_cgroup(const std::string& path) const;
    bool write_file(const std::string& path, const std::string& value) const;
    std::string read_file(const std::string& path) const;
};

} // namespace hermes
