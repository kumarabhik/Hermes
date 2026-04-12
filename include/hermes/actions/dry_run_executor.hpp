#pragma once

#include "hermes/core/types.hpp"

namespace hermes {

class DryRunExecutor {
public:
    InterventionResult execute(const InterventionDecision& decision) const;
};

} // namespace hermes
