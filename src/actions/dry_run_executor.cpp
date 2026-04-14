#include "hermes/actions/dry_run_executor.hpp"

#include <sstream>

namespace hermes {

InterventionResult DryRunExecutor::execute(const InterventionDecision& decision) const {
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
        for (std::size_t index = 0; index < decision.target_pids.size(); ++index) {
            if (index != 0) {
                effect << ",";
            }
            effect << decision.target_pids[index];
        }
    }

    result.system_effect = effect.str();

    switch (decision.action) {
    case ActionKind::Reprioritize:
        result.reversal_condition = "UPS returns to normal band and risk drops to low for 3 consecutive cycles";
        break;
    case ActionKind::Throttle:
        result.reversal_condition = "UPS below elevated band and risk below high for 10s hysteresis window";
        break;
    case ActionKind::TerminateCandidate:
        result.reversal_condition = "none: process termination is irreversible";
        break;
    case ActionKind::Resume:
        result.reversal_condition = "none: resume restores normal scheduling";
        break;
    default:
        result.reversal_condition = "none";
        break;
    }

    return result;
}

} // namespace hermes
