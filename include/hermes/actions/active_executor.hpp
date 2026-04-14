#pragma once

#include "hermes/actions/kill.hpp"
#include "hermes/actions/reprioritize.hpp"
#include "hermes/actions/throttle.hpp"
#include "hermes/core/types.hpp"

namespace hermes {

// ActiveExecutor routes each InterventionDecision to the correct real action executor.
// In observe-only or advisory mode it falls back to dry-run semantics.
// In active-control mode it calls setpriority(), SIGSTOP/SIGCONT, or kill() on Linux.
//
// ThrottleAction is owned here so it can track paused PIDs across calls and
// call resume_all() when the scheduler returns to Recovery state.

struct ActiveExecutorConfig {
    ReprioritizeConfig reprioritize{};
    KillConfig kill{};
};

class ActiveExecutor {
public:
    explicit ActiveExecutor(ActiveExecutorConfig config = {});

    InterventionResult execute(const InterventionDecision& decision);

    // Expose the throttle action so the daemon can call resume_all() on state transition.
    ThrottleAction& throttle_action() {
        return throttle_;
    }

private:
    InterventionResult dry_run_effect(const InterventionDecision& decision) const;

    ReprioritizeAction reprioritize_;
    ThrottleAction throttle_;
    KillAction kill_;
};

} // namespace hermes
