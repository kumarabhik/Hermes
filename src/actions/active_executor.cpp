#include "hermes/actions/active_executor.hpp"

#include <sstream>

namespace hermes {

ActiveExecutor::ActiveExecutor(ActiveExecutorConfig config)
    : reprioritize_(config.reprioritize)
    , kill_(config.kill) {}

InterventionResult ActiveExecutor::execute(const InterventionDecision& decision) {
    // In observe-only or advisory modes, simulate without touching the host.
    if (decision.mode != OperatingMode::ActiveControl || !decision.should_execute) {
        return dry_run_effect(decision);
    }

    switch (decision.action) {
    case ActionKind::Reprioritize:
        return reprioritize_.execute(decision);

    case ActionKind::Throttle:
        return throttle_.pause(decision);

    case ActionKind::Resume:
        return throttle_.resume(decision);

    case ActionKind::TerminateCandidate:
        return kill_.execute(decision);

    case ActionKind::Observe:
    default:
        return dry_run_effect(decision);
    }
}

InterventionResult ActiveExecutor::dry_run_effect(const InterventionDecision& decision) const {
    InterventionResult result;
    result.ts_mono = decision.ts_mono;
    result.success = true;

    std::ostringstream effect;
    if (decision.action == ActionKind::Observe || decision.target_pids.empty()) {
        effect << "no-op";
    } else {
        effect << to_string(decision.mode) << ": would " << to_string(decision.action) << " pid";
        if (decision.target_pids.size() > 1) {
            effect << "s";
        }
        effect << "=";
        for (std::size_t i = 0; i < decision.target_pids.size(); ++i) {
            if (i != 0) {
                effect << ",";
            }
            effect << decision.target_pids[i];
        }
    }
    result.system_effect = effect.str();

    switch (decision.action) {
    case ActionKind::Reprioritize:
        result.reversal_condition = "UPS returns to normal band for 3 consecutive cycles";
        break;
    case ActionKind::Throttle:
        result.reversal_condition = "UPS below elevated band for 10s hysteresis window";
        break;
    case ActionKind::TerminateCandidate:
        result.reversal_condition = "none: process termination is irreversible";
        break;
    default:
        result.reversal_condition = "none";
        break;
    }

    return result;
}

} // namespace hermes
