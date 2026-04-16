#pragma once
// rich_proc_reader.hpp — Optional enriched per-process reader.
//
// Reads additional fields from /proc/<pid>/status that are not available from a
// fast /proc/<pid>/stat scan:
//   VmRSS      — current resident set size (KB)
//   VmSwap     — bytes swapped out (KB)
//   VmPeak     — peak virtual memory usage (KB)
//   VmSize     — current virtual memory size (KB)
//   Threads    — number of threads in the thread group
//   voluntary_ctxt_switches   — voluntary context switches since start
//   nonvoluntary_ctxt_switches — involuntary context switches since start
//
// This reader is compile-guarded behind __linux__ because /proc/status is a
// Linux-only interface. On non-Linux platforms all fields are left at their
// zero defaults.
//
// Usage:
//   RichProcReader reader;
//   RichProcInfo   info;
//   if (reader.read(pid, info)) {
//       std::cout << "VmRSS=" << info.vm_rss_kb << " kB\n";
//   }

#include <cstdint>
#include <string>

namespace hermes {

struct RichProcInfo {
    int pid{-1};

    // Memory counters (kibibytes, matching /proc/<pid>/status units).
    uint64_t vm_peak_kb{0};     // VmPeak
    uint64_t vm_size_kb{0};     // VmSize
    uint64_t vm_rss_kb{0};      // VmRSS
    uint64_t vm_swap_kb{0};     // VmSwap (0 if field absent — older kernels)

    // Threading.
    uint32_t thread_count{0};   // Threads

    // Scheduler statistics.
    uint64_t vol_ctxt_switches{0};    // voluntary_ctxt_switches
    uint64_t invol_ctxt_switches{0};  // nonvoluntary_ctxt_switches

    // Derived helpers.
    double vm_rss_mb()  const { return static_cast<double>(vm_rss_kb)  / 1024.0; }
    double vm_swap_mb() const { return static_cast<double>(vm_swap_kb) / 1024.0; }
    double vm_peak_mb() const { return static_cast<double>(vm_peak_kb) / 1024.0; }
    double vm_size_mb() const { return static_cast<double>(vm_size_kb) / 1024.0; }
};

class RichProcReader {
public:
    RichProcReader() = default;

    // Read /proc/<pid>/status and populate info.
    // Returns true on success; false if the file is missing or unreadable.
    // On non-Linux platforms always returns false.
    bool read(int pid, RichProcInfo& info) const;

    // Convenience: return the last error string if read() returned false.
    const std::string& last_error() const { return last_error_; }

private:
    mutable std::string last_error_;
};

} // namespace hermes
