#pragma once

#include <cstdint>
#include <string>

namespace hermes {

struct ProcStatRecord {
    int pid{-1};
    int ppid{-1};
    char state{'?'};
    long priority{0};
    long nice{0};
    long rss_pages{0};
    uint64_t utime_ticks{0};
    uint64_t stime_ticks{0};
    std::string comm;
    std::string cmdline;
};

class ProcStatReader {
public:
    bool read(int pid, ProcStatRecord& record) const;
};

} // namespace hermes
