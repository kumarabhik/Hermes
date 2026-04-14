#include "hermes/actions/cgroup.hpp"

#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#endif

namespace hermes {

CgroupV2Backend::CgroupV2Backend(std::string cgroup_root)
    : cgroup_root_(std::move(cgroup_root)) {}

std::string CgroupV2Backend::cgroup_path(const std::string& cgroup_name) const {
    return cgroup_root_ + "/" + cgroup_name;
}

bool CgroupV2Backend::is_available() const {
#ifdef __linux__
    std::ifstream test(cgroup_root_ + "/cgroup.controllers");
    return test.is_open();
#else
    return false;
#endif
}

std::string CgroupV2Backend::read_file(const std::string& path) const {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::ostringstream oss;
    oss << file.rdbuf();
    std::string result = oss.str();
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool CgroupV2Backend::write_file(const std::string& path, const std::string& value) const {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << value << '\n';
    return file.good();
}

bool CgroupV2Backend::ensure_cgroup(const std::string& path) const {
#ifdef __linux__
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
#else
    (void)path;
    return false;
#endif
}

std::string CgroupV2Backend::read_control(
    const std::string& cgroup_name,
    const std::string& control_file) const {
    return read_file(cgroup_path(cgroup_name) + "/" + control_file);
}

CgroupResult CgroupV2Backend::set_cpu_max(
    const std::string& cgroup_name,
    uint64_t quota_us,
    uint64_t period_us) {

    CgroupResult result;
    result.cgroup_path = cgroup_path(cgroup_name);
    result.control_file = "cpu.max";
    result.reversal_condition =
        "UPS drops below elevated band; call restore_all() to reset cpu.max to previous value";

#ifndef __linux__
    result.success = false;
    result.error = "cgroup v2 not available on this platform";
    result.applied_value = std::to_string(quota_us) + " " + std::to_string(period_us);
    result.reversal_condition = "simulated only";
    return result;
#else
    if (!ensure_cgroup(result.cgroup_path)) {
        result.error = "failed to create cgroup: " + result.cgroup_path;
        return result;
    }

    const std::string control_path = result.cgroup_path + "/cpu.max";
    result.previous_value = read_file(control_path);

    std::ostringstream value_oss;
    if (quota_us == 0) {
        value_oss << "max " << period_us;
    } else {
        value_oss << quota_us << " " << period_us;
    }
    result.applied_value = value_oss.str();

    if (!write_file(control_path, result.applied_value)) {
        result.error = "failed to write cpu.max: " + control_path;
        return result;
    }

    saved_controls_.push_back({result.cgroup_path, "cpu.max",
                                result.applied_value, result.previous_value});
    result.success = true;
    return result;
#endif
}

CgroupResult CgroupV2Backend::set_memory_high(
    const std::string& cgroup_name,
    uint64_t bytes) {

    CgroupResult result;
    result.cgroup_path = cgroup_path(cgroup_name);
    result.control_file = "memory.high";
    result.reversal_condition =
        "UPS drops below elevated band; call restore_all() to reset memory.high to 'max'";

#ifndef __linux__
    result.success = false;
    result.error = "cgroup v2 not available on this platform";
    result.applied_value = std::to_string(bytes);
    return result;
#else
    if (!ensure_cgroup(result.cgroup_path)) {
        result.error = "failed to create cgroup: " + result.cgroup_path;
        return result;
    }

    const std::string control_path = result.cgroup_path + "/memory.high";
    result.previous_value = read_file(control_path);
    result.applied_value = (bytes == 0) ? "max" : std::to_string(bytes);

    if (!write_file(control_path, result.applied_value)) {
        result.error = "failed to write memory.high: " + control_path;
        return result;
    }

    saved_controls_.push_back({result.cgroup_path, "memory.high",
                                result.applied_value, result.previous_value});
    result.success = true;
    return result;
#endif
}

CgroupResult CgroupV2Backend::set_cpuset(
    const std::string& cgroup_name,
    const std::string& cpu_list) {

    CgroupResult result;
    result.cgroup_path = cgroup_path(cgroup_name);
    result.control_file = "cpuset.cpus";
    result.reversal_condition =
        "Recovery state: call restore_all() to reset cpuset.cpus to original value";

#ifndef __linux__
    result.success = false;
    result.error = "cgroup v2 not available on this platform";
    result.applied_value = cpu_list;
    return result;
#else
    if (!ensure_cgroup(result.cgroup_path)) {
        result.error = "failed to create cgroup: " + result.cgroup_path;
        return result;
    }

    const std::string control_path = result.cgroup_path + "/cpuset.cpus";
    result.previous_value = read_file(control_path);
    result.applied_value = cpu_list;

    if (!write_file(control_path, result.applied_value)) {
        result.error = "failed to write cpuset.cpus: " + control_path;
        return result;
    }

    saved_controls_.push_back({result.cgroup_path, "cpuset.cpus",
                                result.applied_value, result.previous_value});
    result.success = true;
    return result;
#endif
}

CgroupResult CgroupV2Backend::attach_pid(
    const std::string& cgroup_name,
    int pid) {

    CgroupResult result;
    result.cgroup_path = cgroup_path(cgroup_name);
    result.control_file = "cgroup.procs";
    result.applied_value = std::to_string(pid);
    result.reversal_condition = "move pid back to parent cgroup to remove restriction";

#ifndef __linux__
    result.success = false;
    result.error = "cgroup v2 not available on this platform";
    return result;
#else
    if (!ensure_cgroup(result.cgroup_path)) {
        result.error = "failed to create cgroup: " + result.cgroup_path;
        return result;
    }

    const std::string procs_path = result.cgroup_path + "/cgroup.procs";
    if (!write_file(procs_path, result.applied_value)) {
        result.error = "failed to attach pid " + result.applied_value + " to " + procs_path;
        return result;
    }

    result.success = true;
    return result;
#endif
}

int CgroupV2Backend::restore_all() {
    int restored = 0;
    for (auto it = saved_controls_.rbegin(); it != saved_controls_.rend(); ++it) {
        const std::string full_path = it->cgroup_path + "/" + it->control_file;
        const std::string value = it->previous_value.empty() ? "max" : it->previous_value;
        if (write_file(full_path, value)) {
            ++restored;
        }
    }
    saved_controls_.clear();
    return restored;
}

} // namespace hermes
