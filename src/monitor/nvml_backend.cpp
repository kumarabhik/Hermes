// nvml_backend.cpp — Runtime NVML integration via dlopen/LoadLibrary.
//
// We never include nvml.h so the binary stays portable to non-GPU hosts.
// All types needed from the NVML ABI are redeclared locally using the
// documented layout (stable across driver generations since NVML 5.x).

#include "hermes/monitor/nvml_backend.hpp"
#include "hermes/core/types.hpp"

#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#  include <dlfcn.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace hermes {
namespace {

// ---------------------------------------------------------------------------
// Minimal NVML ABI redeclarations (stable layout, no nvml.h required)
// ---------------------------------------------------------------------------

using nvmlReturn_t  = int;
using nvmlDevice_t  = void*;

static constexpr nvmlReturn_t NVML_SUCCESS          = 0;
static constexpr unsigned int NVML_TEMPERATURE_GPU  = 0;
static constexpr unsigned int NVML_DEVICE_NAME_SIZE = 96;
static constexpr unsigned int NVML_MAX_PROCESSES    = 64;

struct NvmlMemory {
    unsigned long long total{0};
    unsigned long long free{0};
    unsigned long long used{0};
};

struct NvmlUtilization {
    unsigned int gpu{0};
    unsigned int memory{0};
};

struct NvmlProcessInfo {
    unsigned int pid{0};
    unsigned long long usedGpuMemory{0};
};

// ---------------------------------------------------------------------------
// Function pointer typedefs
// ---------------------------------------------------------------------------

using PfnInit          = nvmlReturn_t (*)();
using PfnShutdown      = nvmlReturn_t (*)();
using PfnDeviceCount   = nvmlReturn_t (*)(unsigned int*);
using PfnDeviceHandle  = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using PfnDeviceName    = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using PfnMemoryInfo    = nvmlReturn_t (*)(nvmlDevice_t, NvmlMemory*);
using PfnUtilization   = nvmlReturn_t (*)(nvmlDevice_t, NvmlUtilization*);
using PfnTemperature   = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);
using PfnPowerUsage    = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using PfnComputeProcs  = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, NvmlProcessInfo*);
using PfnGraphicsProcs = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, NvmlProcessInfo*);

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

void* open_library(const char* name) {
#if defined(__linux__)
    return dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#elif defined(_WIN32)
    return static_cast<void*>(LoadLibraryA(name));
#else
    (void)name;
    return nullptr;
#endif
}

void close_library(void* handle) {
    if (!handle) return;
#if defined(__linux__)
    dlclose(handle);
#elif defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#endif
}

void* get_symbol(void* handle, const char* name) {
    if (!handle) return nullptr;
#if defined(__linux__)
    return dlsym(handle, name);
#elif defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    (void)handle; (void)name;
    return nullptr;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// NvmlBackend implementation
// ---------------------------------------------------------------------------

NvmlBackend::NvmlBackend() {
    if (!load_library()) return;
    if (!init_nvml()) {
        unload_library();
        return;
    }

    // Query device count.
    auto fn = reinterpret_cast<PfnDeviceCount>(fn_device_count_);
    unsigned int count = 0;
    if (fn && fn(&count) == NVML_SUCCESS) {
        device_count_ = count;
    }

    available_ = true;
}

NvmlBackend::~NvmlBackend() {
    if (available_) {
        shutdown_nvml();
        unload_library();
    }
}

bool NvmlBackend::load_library() {
#if defined(__linux__)
    // Try versioned names first (common on CUDA installations), then generic.
    static const char* candidates[] = {
        "libnvidia-ml.so.1",
        "libnvidia-ml.so",
        nullptr
    };
    for (const char** p = candidates; *p; ++p) {
        lib_handle_ = open_library(*p);
        if (lib_handle_) break;
    }
#elif defined(_WIN32)
    lib_handle_ = open_library("nvml.dll");
#endif

    if (!lib_handle_) {
        unavailable_reason_ = "NVML shared library not found on this host";
        return false;
    }

    // Resolve function pointers.  We try the _v2 versioned symbols first
    // (available since driver ~304), then fall back to the unversioned names.
    auto resolve = [&](const char* v2, const char* v1) -> void* {
        void* sym = get_symbol(lib_handle_, v2);
        if (!sym) sym = get_symbol(lib_handle_, v1);
        return sym;
    };

    fn_init_          = resolve("nvmlInit_v2",                "nvmlInit");
    fn_shutdown_      = resolve("nvmlShutdown",               "nvmlShutdown");
    fn_device_count_  = resolve("nvmlDeviceGetCount_v2",      "nvmlDeviceGetCount");
    fn_device_handle_ = resolve("nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    fn_device_name_   = resolve("nvmlDeviceGetName",          "nvmlDeviceGetName");
    fn_memory_info_   = resolve("nvmlDeviceGetMemoryInfo",    "nvmlDeviceGetMemoryInfo");
    fn_utilization_   = resolve("nvmlDeviceGetUtilizationRates", "nvmlDeviceGetUtilizationRates");
    fn_temperature_   = resolve("nvmlDeviceGetTemperature",   "nvmlDeviceGetTemperature");
    fn_power_usage_   = resolve("nvmlDeviceGetPowerUsage",    "nvmlDeviceGetPowerUsage");
    fn_compute_procs_ = resolve("nvmlDeviceGetComputeRunningProcesses",
                                "nvmlDeviceGetComputeRunningProcesses");
    fn_graphics_procs_= resolve("nvmlDeviceGetGraphicsRunningProcesses",
                                "nvmlDeviceGetGraphicsRunningProcesses");

    if (!fn_init_ || !fn_shutdown_ || !fn_device_count_ || !fn_device_handle_) {
        unavailable_reason_ = "NVML library found but required symbols are missing";
        close_library(lib_handle_);
        lib_handle_ = nullptr;
        return false;
    }

    return true;
}

void NvmlBackend::unload_library() {
    close_library(lib_handle_);
    lib_handle_ = nullptr;
}

bool NvmlBackend::init_nvml() {
    auto fn = reinterpret_cast<PfnInit>(fn_init_);
    if (!fn) {
        unavailable_reason_ = "nvmlInit symbol not resolved";
        return false;
    }
    const nvmlReturn_t rc = fn();
    if (rc != NVML_SUCCESS) {
        unavailable_reason_ = "nvmlInit returned error code " + std::to_string(rc);
        return false;
    }
    return true;
}

void NvmlBackend::shutdown_nvml() {
    auto fn = reinterpret_cast<PfnShutdown>(fn_shutdown_);
    if (fn) fn();
}

bool NvmlBackend::query_device(unsigned int device_index, GpuDeviceStats& out) {
    if (!available_) return false;

    // Get device handle.
    auto fn_handle = reinterpret_cast<PfnDeviceHandle>(fn_device_handle_);
    if (!fn_handle) return false;

    nvmlDevice_t handle = nullptr;
    if (fn_handle(device_index, &handle) != NVML_SUCCESS || !handle) return false;

    out = GpuDeviceStats{};
    out.device_index = device_index;

    // Name.
    if (fn_device_name_) {
        char name_buf[NVML_DEVICE_NAME_SIZE] = {};
        auto fn = reinterpret_cast<PfnDeviceName>(fn_device_name_);
        if (fn(handle, name_buf, NVML_DEVICE_NAME_SIZE) == NVML_SUCCESS) {
            out.name = name_buf;
        }
    }

    // Memory.
    if (fn_memory_info_) {
        NvmlMemory mem{};
        auto fn = reinterpret_cast<PfnMemoryInfo>(fn_memory_info_);
        if (fn(handle, &mem) == NVML_SUCCESS) {
            out.vram_total_bytes = mem.total;
            out.vram_used_bytes  = mem.used;
            out.vram_free_bytes  = mem.free;
        }
    }

    // Utilization.
    if (fn_utilization_) {
        NvmlUtilization util{};
        auto fn = reinterpret_cast<PfnUtilization>(fn_utilization_);
        if (fn(handle, &util) == NVML_SUCCESS) {
            out.gpu_util_pct = util.gpu;
            out.mem_util_pct = util.memory;
        }
    }

    // Temperature.
    if (fn_temperature_) {
        unsigned int temp = 0;
        auto fn = reinterpret_cast<PfnTemperature>(fn_temperature_);
        if (fn(handle, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
            out.temperature_c = temp;
        }
    }

    // Power (milliwatts).
    if (fn_power_usage_) {
        unsigned int power_mw = 0;
        auto fn = reinterpret_cast<PfnPowerUsage>(fn_power_usage_);
        if (fn(handle, &power_mw) == NVML_SUCCESS) {
            out.power_draw_mw = power_mw;
        }
    }

    out.valid = true;
    return true;
}

bool NvmlBackend::query_processes(unsigned int device_index,
                                  std::vector<GpuProcessStats>& out) {
    if (!available_) return false;

    auto fn_handle = reinterpret_cast<PfnDeviceHandle>(fn_device_handle_);
    if (!fn_handle) return false;

    nvmlDevice_t handle = nullptr;
    if (fn_handle(device_index, &handle) != NVML_SUCCESS || !handle) return false;

    out.clear();

    auto collect = [&](void* fn_ptr) {
        if (!fn_ptr) return;
        auto fn = reinterpret_cast<PfnComputeProcs>(fn_ptr);
        NvmlProcessInfo infos[NVML_MAX_PROCESSES] = {};
        unsigned int count = NVML_MAX_PROCESSES;
        if (fn(handle, &count, infos) != NVML_SUCCESS) return;
        for (unsigned int i = 0; i < count; ++i) {
            GpuProcessStats s;
            s.pid = infos[i].pid;
            s.used_gpu_memory_bytes = infos[i].usedGpuMemory;
            s.valid = true;
            out.push_back(s);
        }
    };

    collect(fn_compute_procs_);
    collect(fn_graphics_procs_);

    // De-duplicate by PID (graphics and compute may both report the same process).
    std::sort(out.begin(), out.end(),
              [](const GpuProcessStats& a, const GpuProcessStats& b) {
                  return a.pid < b.pid;
              });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const GpuProcessStats& a, const GpuProcessStats& b) {
                              return a.pid == b.pid;
                          }),
              out.end());

    return true;
}

bool NvmlBackend::fill_sample(PressureSample& sample) {
    if (!available_ || device_count_ == 0) return false;

    // Aggregate across all devices:
    //   vram_used/total/free  — summed (total VRAM pressure across all GPUs)
    //   gpu_util_pct          — averaged (mean utilisation across all GPUs)
    constexpr double kBytesPerMb = 1048576.0;

    double sum_vram_used  = 0.0;
    double sum_vram_total = 0.0;
    double sum_vram_free  = 0.0;
    double sum_gpu_util   = 0.0;
    unsigned int valid_devices = 0;

    for (unsigned int dev = 0; dev < device_count_; ++dev) {
        GpuDeviceStats stats;
        if (!query_device(dev, stats) || !stats.valid) continue;
        sum_vram_used  += static_cast<double>(stats.vram_used_bytes)  / kBytesPerMb;
        sum_vram_total += static_cast<double>(stats.vram_total_bytes) / kBytesPerMb;
        sum_vram_free  += static_cast<double>(stats.vram_free_bytes)  / kBytesPerMb;
        sum_gpu_util   += static_cast<double>(stats.gpu_util_pct);
        ++valid_devices;
    }

    if (valid_devices == 0) return false;

    sample.vram_used_mb  = sum_vram_used;
    sample.vram_total_mb = sum_vram_total;
    sample.vram_free_mb  = sum_vram_free;
    sample.gpu_util_pct  = sum_gpu_util / static_cast<double>(valid_devices);
    return true;
}

bool NvmlBackend::query_all_processes(std::vector<GpuProcessStats>& out) {
    if (!available_ || device_count_ == 0) return false;

    out.clear();
    bool any = false;
    for (unsigned int dev = 0; dev < device_count_; ++dev) {
        std::vector<GpuProcessStats> dev_procs;
        if (!query_processes(dev, dev_procs)) continue;
        for (auto& p : dev_procs) {
            out.push_back(p);
        }
        any = true;
    }

    if (!any) return false;

    // Merge entries for the same PID across devices (sum their memory usage).
    std::sort(out.begin(), out.end(),
              [](const GpuProcessStats& a, const GpuProcessStats& b) {
                  return a.pid < b.pid;
              });
    std::vector<GpuProcessStats> merged;
    for (auto& p : out) {
        if (!merged.empty() && merged.back().pid == p.pid) {
            merged.back().used_gpu_memory_bytes += p.used_gpu_memory_bytes;
        } else {
            merged.push_back(p);
        }
    }
    out = std::move(merged);
    return true;
}

} // namespace hermes
