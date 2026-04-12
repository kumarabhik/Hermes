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
    return result;
}

} // namespace hermes
