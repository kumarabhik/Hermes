#include "hermes/actions/throttle.hpp"

#include <sstream>

#ifdef __linux__
#include <csignal>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#endif

namespace hermes {

InterventionResult ThrottleAction::pause(const InterventionDecision& decision) {
    InterventionResult result;
    result.ts_mono = decision.ts_mono;
    last_records_.clear();

    if (decision.target_pids.empty()) {
        result.success = true;
        result.system_effect = "no-op: no target pids";
        result.reversal_condition = "call resume() when UPS below elevated band for 10s";
        return result;
    }

    std::ostringstream effect;
    bool any_failure = false;

    for (const int pid : decision.target_pids) {
        ThrottleRecord record;
        record.pid = pid;

        if (paused_pids_.count(pid) > 0) {
            record.paused = false;
            record.error = "already paused";
            effect << "[pid=" << pid << " skip: already paused]";
            last_records_.push_back(record);
            continue;
        }

#ifdef __linux__
        if (::kill(pid, SIGSTOP) != 0) {
            record.error = std::string("SIGSTOP failed: ") + std::strerror(errno);
            record.paused = false;
            any_failure = true;
            effect << "[pid=" << pid << " err=" << record.error << "]";
        } else {
            record.paused = true;
            paused_pids_.insert(pid);
            effect << "[pid=" << pid << " SIGSTOP sent]";
        }
#else
        record.paused = false;
        record.error = "SIGSTOP not available on this platform";
        effect << "[pid=" << pid << " simulated: SIGSTOP]";
#endif
        last_records_.push_back(record);
    }

    result.success = !any_failure;
    result.system_effect = "throttle-pause: " + effect.str();
    result.reversal_condition =
        "UPS below elevated band and risk below high for 10s; call resume() to send SIGCONT";
    return result;
}

InterventionResult ThrottleAction::resume(const InterventionDecision& decision) {
    InterventionResult result;
    result.ts_mono = decision.ts_mono;
    last_records_.clear();

    if (decision.target_pids.empty()) {
        result.success = true;
        result.system_effect = "no-op: no target pids";
        result.reversal_condition = "none";
        return result;
    }

    std::ostringstream effect;
    bool any_failure = false;

    for (const int pid : decision.target_pids) {
        ThrottleRecord record;
        record.pid = pid;

#ifdef __linux__
        if (::kill(pid, SIGCONT) != 0) {
            record.error = std::string("SIGCONT failed: ") + std::strerror(errno);
            record.resumed = false;
            any_failure = true;
            effect << "[pid=" << pid << " err=" << record.error << "]";
        } else {
            record.resumed = true;
            paused_pids_.erase(pid);
            effect << "[pid=" << pid << " SIGCONT sent]";
        }
#else
        record.resumed = false;
        record.error = "SIGCONT not available on this platform";
        effect << "[pid=" << pid << " simulated: SIGCONT]";
#endif
        last_records_.push_back(record);
    }

    result.success = !any_failure;
    result.reverted = true;
    result.system_effect = "throttle-resume: " + effect.str();
    result.reversal_condition = "none: resume restores normal scheduling";
    return result;
}

bool ThrottleAction::is_paused(int pid) const {
    return paused_pids_.count(pid) > 0;
}

InterventionResult ThrottleAction::resume_all(uint64_t ts_mono) {
    InterventionDecision synthetic;
    synthetic.ts_mono = ts_mono;
    synthetic.action = ActionKind::Resume;
    synthetic.mode = OperatingMode::ActiveControl;
    synthetic.target_pids.assign(paused_pids_.begin(), paused_pids_.end());
    return resume(synthetic);
}

} // namespace hermes
