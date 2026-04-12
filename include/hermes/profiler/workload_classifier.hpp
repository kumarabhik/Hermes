#pragma once

#include "hermes/core/types.hpp"

#include <vector>

namespace hermes {

class WorkloadClassifier {
public:
    void classify(std::vector<ProcessSnapshot>& processes) const;
};

} // namespace hermes
