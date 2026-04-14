#pragma once

// LatencyProbe: per-loop latency tracker for the hermesd policy loop.
//
// Call begin_loop() at the top of each policy iteration and end_loop() at the
// bottom. The probe accumulates elapsed milliseconds in a ring buffer and
// computes p50/p95/p99/max on demand.
//
// write() serialises a latency_summary.json file alongside the run directory
// artifacts. The daemon calls this at shutdown (or periodically).
//
// Thread safety: NOT thread-safe. Designed to be used from a single thread
// (the policy thread). External synchronisation required if called from
// multiple threads.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hermes {

class LatencyProbe {
public:
    // max_samples: ring buffer capacity. Oldest entries are overwritten when
    // the buffer is full. Default 4096 samples ≈ ~34 minutes at 500 ms cadence.
    explicit LatencyProbe(std::size_t max_samples = 4096);

    // Call at the start of each policy loop iteration.
    void begin_loop();

    // Call at the end of each policy loop iteration. Records elapsed ms since
    // the last begin_loop() call.
    void end_loop();

    // Percentile statistics (milliseconds). Returns 0 if no samples recorded.
    double p50_ms() const;
    double p95_ms() const;
    double p99_ms() const;
    double max_ms() const;
    double mean_ms() const;
    uint64_t count() const;

    // Write latency_summary.json to path. Returns false on I/O error.
    bool write(const std::filesystem::path& path, std::string& error) const;

private:
    double percentile(double p) const; // p in [0,1]

    std::size_t max_samples_;
    std::vector<double> latencies_ms_; // ring buffer (push_back, trim front)
    std::chrono::steady_clock::time_point loop_start_;
    bool in_loop_{false};
};

} // namespace hermes
