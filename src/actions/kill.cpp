#include "hermes/actions/kill.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#ifdef __linux__
#include <csignal>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hermes {

KillAction::KillAction(KillConfig config)
    : config_(std::move(config)) {
#ifdef __linux__
    self_pid_ = static_cast<int>(::getpid());
#endif
}

bool KillAction::is_guardrailed(int pid, const std::string& cmd, std::string& reason) const {
    if (pid <= 1) {
        reason = "pid <= 1 is always protected";
        return true;
    }

    if (pid == self_pid_ && self_pid_ > 0) {
        reason = "cannot terminate hermesd itself";
        return true;
    }

    if (config_.protected_pids.count(pid) > 0) {
        reason = "pid is in protected_pids config";
        return true;
    }

    for (const std::string& pattern : config_.protected_name_patterns) {
        if (!pattern.empty() && cmd.find(pattern) != std::string::npos) {
            reason = "cmd matches protected pattern: " + pattern;
            return true;
        }
    }

    return false;
}

void KillAction::update_placement_data(
    const std::unordered_map<int, int>&    pid_device,
    const std::unordered_map<int, double>& device_util) {
    config_.pid_device   = pid_device;
    config_.device_util  = device_util;
}

std::vector<int> KillAction::sort_by_placement(const std::vector<int>& pids) const {
    if (!config_.placement_aware_kills
            || config_.device_util.empty()
            || config_.pid_device.empty()) {
        return pids;
    }

    // Find the device with the highest utilization.
    int hottest_device = -1;
    double best_util   = -1.0;
    for (const auto& kv : config_.device_util) {
        if (kv.second > best_util) {
            best_util      = kv.second;
            hottest_device = kv.first;
        }
    }

    if (hottest_device < 0) return pids;

    // Sort: pids on the hottest device first, others afterward (stable relative order).
    std::vector<int> sorted = pids;
    std::stable_partition(sorted.begin(), sorted.end(), [&](int pid) {
        const auto it = config_.pid_device.find(pid);
        return it != config_.pid_device.end() && it->second == hottest_device;
    });
    return sorted;
}

InterventionResult KillAction::execute(const InterventionDecision& decision) {
    InterventionResult result;
    result.ts_mono = decision.ts_mono;
    last_records_.clear();

    if (decision.target_pids.empty()) {
        result.success = true;
        result.system_effect = "no-op: no target pids";
        result.reversal_condition = "none: process termination is irreversible";
        return result;
    }

    // Apply placement-aware re-ordering if enabled.
    const std::vector<int> ordered_pids = sort_by_placement(decision.target_pids);

    std::ostringstream effect;
    bool any_success = false;
    bool any_failure = false;

    for (const int pid : ordered_pids) {
        KillRecord record;
        record.pid = pid;

        std::string guard_reason;
        if (is_guardrailed(pid, "", guard_reason)) {
            record.reject_reason = guard_reason;
            record.terminated = false;
            effect << "[pid=" << pid << " BLOCKED: " << guard_reason << "]";
            last_records_.push_back(record);
            any_failure = true;
            continue;
        }

#ifdef __linux__
        const int sig = config_.use_sigkill ? SIGKILL : SIGTERM;
        record.signal_sent = config_.use_sigkill ? "SIGKILL" : "SIGTERM";

        if (::kill(pid, sig) != 0) {
            record.error = std::string("kill(") + record.signal_sent + ") failed: " + std::strerror(errno);
            record.terminated = false;
            any_failure = true;
            effect << "[pid=" << pid << " err=" << record.error << "]";
        } else {
            record.terminated = true;
            any_success = true;
            effect << "[pid=" << pid << " " << record.signal_sent << " sent]";
        }
#else
        record.signal_sent = config_.use_sigkill ? "SIGKILL" : "SIGTERM";
        record.terminated = false;
        record.error = "kill not available on this platform";
        effect << "[pid=" << pid << " simulated: " << record.signal_sent << "]";
#endif
        last_records_.push_back(record);
    }

    result.success = any_success && !any_failure;
    result.system_effect = "terminate: " + effect.str();
    result.reversal_condition = "none: process termination is irreversible";
    return result;
}

} // namespace hermes
