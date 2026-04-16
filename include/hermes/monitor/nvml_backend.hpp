#pragma once

// nvml_backend.hpp — Optional direct NVML integration for GPU stats.
//
// Provides a faster, lower-overhead alternative to the nvidia-smi subprocess
// path. Loaded at runtime via dlopen (Linux) / LoadLibrary (Windows) so the
// binary does not hard-link against libnvidia-ml and remains portable to
// non-GPU hosts.
//
// Usage:
//   NvmlBackend nvml;
//   if (nvml.available()) {
//       GpuDeviceStats stats;
//       nvml.query_device(0, stats);
//   }
//
// If NVML is not available the caller should fall back to the nvidia-smi path.

#include "hermes/core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hermes {

struct GpuDeviceStats {
    unsigned int device_index{0};
    std::string  name;
    uint64_t     vram_total_bytes{0};
    uint64_t     vram_used_bytes{0};
    uint64_t     vram_free_bytes{0};
    unsigned int gpu_util_pct{0};     // 0–100
    unsigned int mem_util_pct{0};     // 0–100
    unsigned int temperature_c{0};
    unsigned int power_draw_mw{0};
    bool         valid{false};
};

struct GpuProcessStats {
    unsigned int pid{0};
    uint64_t     used_gpu_memory_bytes{0};
    bool         valid{false};
};

class NvmlBackend {
public:
    NvmlBackend();
    ~NvmlBackend();

    // Returns true if NVML was successfully loaded and initialised.
    bool available() const { return available_; }

    // Human-readable reason why NVML is not available (empty if available).
    const std::string& unavailable_reason() const { return unavailable_reason_; }

    // Query aggregate stats for a single device (index 0 = first GPU).
    bool query_device(unsigned int device_index, GpuDeviceStats& out);

    // Query per-process GPU memory usage for a device.
    bool query_processes(unsigned int device_index,
                         std::vector<GpuProcessStats>& out);

    // Number of GPUs visible via NVML.
    unsigned int device_count() const { return device_count_; }

    // Fill a PressureSample's GPU fields aggregated across all devices.
    // vram_used/total/free are summed; gpu_util_pct is averaged.
    // Returns false and leaves sample unchanged if NVML is not available.
    bool fill_sample(PressureSample& sample);

    // Query per-process GPU memory usage across ALL devices, merging entries
    // for the same PID (their memory contributions are summed).
    bool query_all_processes(std::vector<GpuProcessStats>& out);

private:
    bool load_library();
    void unload_library();
    bool init_nvml();
    void shutdown_nvml();

    bool          available_{false};
    std::string   unavailable_reason_;
    unsigned int  device_count_{0};

    // Opaque handle to the loaded shared library.
    void* lib_handle_{nullptr};

    // Function pointer types (minimal subset of NVML API needed).
    // We declare them as void* and cast at call sites to avoid including
    // nvml.h, which may not be present on the build machine.
    void* fn_init_{nullptr};
    void* fn_shutdown_{nullptr};
    void* fn_device_count_{nullptr};
    void* fn_device_handle_{nullptr};
    void* fn_device_name_{nullptr};
    void* fn_memory_info_{nullptr};
    void* fn_utilization_{nullptr};
    void* fn_temperature_{nullptr};
    void* fn_power_usage_{nullptr};
    void* fn_compute_procs_{nullptr};
    void* fn_graphics_procs_{nullptr};
};

} // namespace hermes
