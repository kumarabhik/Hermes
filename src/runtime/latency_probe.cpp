#include "hermes/runtime/latency_probe.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace hermes {

LatencyProbe::LatencyProbe(std::size_t max_samples)
    : max_samples_(max_samples) {
    latencies_ms_.reserve(std::min(max_samples, std::size_t{4096}));
}

void LatencyProbe::begin_loop() {
    loop_start_ = std::chrono::steady_clock::now();
    in_loop_ = true;
}

void LatencyProbe::end_loop() {
    if (!in_loop_) return;
    const auto elapsed = std::chrono::steady_clock::now() - loop_start_;
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    in_loop_ = false;

    if (latencies_ms_.size() >= max_samples_) {
        // Trim oldest quarter to amortise the erase cost
        const std::size_t trim = max_samples_ / 4;
        latencies_ms_.erase(latencies_ms_.begin(),
                            latencies_ms_.begin() + static_cast<std::ptrdiff_t>(trim));
    }
    latencies_ms_.push_back(ms);
}

uint64_t LatencyProbe::count() const {
    return static_cast<uint64_t>(latencies_ms_.size());
}

double LatencyProbe::percentile(double p) const {
    if (latencies_ms_.empty()) return 0.0;
    std::vector<double> sorted = latencies_ms_;
    std::sort(sorted.begin(), sorted.end());
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

double LatencyProbe::p50_ms() const { return percentile(0.50); }
double LatencyProbe::p95_ms() const { return percentile(0.95); }
double LatencyProbe::p99_ms() const { return percentile(0.99); }

double LatencyProbe::max_ms() const {
    if (latencies_ms_.empty()) return 0.0;
    return *std::max_element(latencies_ms_.begin(), latencies_ms_.end());
}

double LatencyProbe::mean_ms() const {
    if (latencies_ms_.empty()) return 0.0;
    const double sum = std::accumulate(latencies_ms_.begin(), latencies_ms_.end(), 0.0);
    return sum / static_cast<double>(latencies_ms_.size());
}

bool LatencyProbe::write(const std::filesystem::path& path, std::string& error) const {
    try {
        std::filesystem::create_directories(path.parent_path());
    } catch (const std::exception& ex) {
        error = std::string("failed to create directory: ") + ex.what();
        return false;
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        error = "cannot open " + path.string();
        return false;
    }

    f << std::fixed << std::setprecision(3);
    f << "{\n"
      << "  \"sample_count\": " << latencies_ms_.size() << ",\n"
      << "  \"mean_ms\": "      << mean_ms()  << ",\n"
      << "  \"p50_ms\": "       << p50_ms()   << ",\n"
      << "  \"p95_ms\": "       << p95_ms()   << ",\n"
      << "  \"p99_ms\": "       << p99_ms()   << ",\n"
      << "  \"max_ms\": "       << max_ms()   << "\n"
      << "}\n";

    if (!f.good()) {
        error = "write error on " + path.string();
        return false;
    }
    return true;
}

} // namespace hermes
