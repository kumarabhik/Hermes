// rich_proc_reader.cpp — Enriched per-process reader from /proc/<pid>/status.
//
// Reads VmRSS, VmSwap, VmPeak, VmSize, Threads, and context-switch counters.
// All paths are compile-guarded behind __linux__ — on Windows/macOS the read()
// stub always returns false so callers can safely elide the enriched path.

#include "hermes/monitor/rich_proc_reader.hpp"

#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#endif

namespace hermes {

#ifdef __linux__

namespace {

// Parse a single "Key:\t<value> kB\n" line from /proc/<pid>/status.
// Returns true and stores the value when the key matches.
bool parse_kb_field(const char* line, const char* key, uint64_t& out) {
    const std::size_t klen = std::strlen(key);
    if (std::strncmp(line, key, klen) != 0) return false;
    // Skip past key, colon, and any whitespace.
    const char* p = line + klen;
    while (*p == ':' || *p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

bool parse_u32_field(const char* line, const char* key, uint32_t& out) {
    const std::size_t klen = std::strlen(key);
    if (std::strncmp(line, key, klen) != 0) return false;
    const char* p = line + klen;
    while (*p == ':' || *p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 10);
    if (end == p) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

} // namespace

bool RichProcReader::read(int pid, RichProcInfo& info) const {
    if (pid <= 0) {
        last_error_ = "invalid pid";
        return false;
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", pid);

    FILE* f = std::fopen(path, "r");
    if (!f) {
        last_error_ = std::string("cannot open ") + path + ": " + std::strerror(errno);
        return false;
    }

    info = RichProcInfo{};
    info.pid = pid;

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        uint64_t u64 = 0;
        uint32_t u32 = 0;

        if (parse_kb_field(line, "VmPeak", u64)) { info.vm_peak_kb = u64; continue; }
        if (parse_kb_field(line, "VmSize", u64)) { info.vm_size_kb = u64; continue; }
        if (parse_kb_field(line, "VmRSS",  u64)) { info.vm_rss_kb  = u64; continue; }
        if (parse_kb_field(line, "VmSwap", u64)) { info.vm_swap_kb = u64; continue; }

        if (parse_u32_field(line, "Threads", u32)) { info.thread_count = u32; continue; }

        if (parse_kb_field(line, "voluntary_ctxt_switches",    u64)) {
            info.vol_ctxt_switches = u64;   continue;
        }
        if (parse_kb_field(line, "nonvoluntary_ctxt_switches", u64)) {
            info.invol_ctxt_switches = u64; continue;
        }
    }

    std::fclose(f);
    last_error_.clear();
    return true;
}

#else // not Linux

bool RichProcReader::read(int /*pid*/, RichProcInfo& /*info*/) const {
    last_error_ = "RichProcReader not supported on non-Linux platforms";
    return false;
}

#endif // __linux__

} // namespace hermes
