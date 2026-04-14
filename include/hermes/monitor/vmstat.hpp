#pragma once

#include "hermes/core/types.hpp"

#include <string>

namespace hermes {

// VmstatMonitor reads /proc/vmstat to extract pgmajfault and pgfault counters.
// These are cumulative since boot; the daemon should take deltas between samples
// to get per-interval fault rates. This is a Tier A/B/C signal (available everywhere).
//
// The counters populate PressureSample::vmstat_pgmajfault and vmstat_pgfault.
// A delta > 0 in pgmajfault during a high-UPS window is strong evidence of
// memory pressure causing disk-backed page reclaim.
class VmstatMonitor {
public:
    VmstatMonitor() = default;

    // Read /proc/vmstat and store cumulative fault counters in sample.
    // Returns true on success, false if the file is unavailable.
    bool update_sample(PressureSample& sample);

    // Convenience: compute the per-interval major fault delta between two samples.
    static uint64_t major_fault_delta(const PressureSample& prev, const PressureSample& cur) {
        if (cur.vmstat_pgmajfault >= prev.vmstat_pgmajfault) {
            return cur.vmstat_pgmajfault - prev.vmstat_pgmajfault;
        }
        return 0;
    }

    static uint64_t minor_fault_delta(const PressureSample& prev, const PressureSample& cur) {
        if (cur.vmstat_pgfault >= prev.vmstat_pgfault) {
            return cur.vmstat_pgfault - prev.vmstat_pgfault;
        }
        return 0;
    }

private:
    std::string vmstat_path_ = "/proc/vmstat";
};

} // namespace hermes
