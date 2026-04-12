#include "hermes/profiler/proc_stat.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hermes {
namespace {

std::string read_cmdline(int pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (char& ch : raw) {
        if (ch == '\0') {
            ch = ' ';
        }
    }

    while (!raw.empty() && raw.back() == ' ') {
        raw.pop_back();
    }

    return raw;
}

} // namespace

bool ProcStatReader::read(int pid, ProcStatRecord& record) const {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) {
        return false;
    }

    const std::size_t open_paren = line.find('(');
    const std::size_t close_paren = line.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren) {
        return false;
    }

    try {
        record.pid = std::stoi(line.substr(0, open_paren));
    } catch (...) {
        return false;
    }

    record.comm = line.substr(open_paren + 1, close_paren - open_paren - 1);
    record.cmdline = read_cmdline(pid);

    const std::string remainder = line.substr(close_paren + 2);
    std::istringstream iss(remainder);
    std::vector<std::string> fields;
    std::string field;
    while (iss >> field) {
        fields.push_back(field);
    }

    if (fields.size() < 22) {
        return false;
    }

    try {
        record.state = fields[0].empty() ? '?' : fields[0][0];
        record.ppid = std::stoi(fields[1]);
        record.utime_ticks = static_cast<uint64_t>(std::stoull(fields[11]));
        record.stime_ticks = static_cast<uint64_t>(std::stoull(fields[12]));
        record.priority = std::stol(fields[15]);
        record.nice = std::stol(fields[16]);
        record.rss_pages = std::stol(fields[21]);
    } catch (...) {
        return false;
    }

    return true;
}

} // namespace hermes
