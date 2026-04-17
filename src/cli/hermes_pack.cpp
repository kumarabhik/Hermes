// hermes_pack: portable evidence bundle packaging tool.
//
// Copies all artifacts from a Hermes run directory into a self-contained
// bundle directory and writes a bundle_manifest.json with file sizes and
// FNV-1a hashes for integrity checking.
//
// Usage:
//   hermes_pack <run-dir> [<run-dir> ...]
//   hermes_pack <run-dir> --output-dir artifacts/bundles/my-bundle
//   hermes_pack <run-dir> --list
//
// Output layout (default: artifacts/evidence_bundles/<run_id>/):
//   bundle_manifest.json   — index of all bundled files + sizes + hashes
//   run_metadata.json      — copied from run
//   config_snapshot.yaml   — copied from run
//   telemetry_quality.json — copied from run
//   samples.ndjson         — copied from run
//   scores.ndjson          — ...
//   predictions.ndjson
//   decisions.ndjson
//   actions.ndjson
//   events.ndjson
//   processes.ndjson
//   replay_summary.json    — copied if present
//   summary.csv            — copied if present
//   eval_summary.json      — copied if present
//
// The bundle is self-contained: all fields needed for hermes_replay are present.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// FNV-1a 64-bit hash of file contents.
uint64_t fnv1a_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return 0;
    uint64_t h = 14695981039346656037ull;
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string hex64(uint64_t v) {
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << v;
    return os.str();
}

std::string now_iso() {
    const auto t = std::chrono::system_clock::now();
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        t.time_since_epoch()).count();
    const std::time_t tt = static_cast<std::time_t>(ts);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
    return buf;
}

std::string jstr(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":\"";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    const auto start = pos + search.size();
    const auto end = json.find('"', start);
    return end == std::string::npos ? "" : json.substr(start, end - start);
}

// ---- Known artifact filenames to bundle ----

const std::vector<std::string> ARTIFACT_FILES = {
    "run_metadata.json",
    "config_snapshot.yaml",
    "telemetry_quality.json",
    "samples.ndjson",
    "scores.ndjson",
    "predictions.ndjson",
    "decisions.ndjson",
    "actions.ndjson",
    "events.ndjson",
    "processes.ndjson",
    "replay_summary.json",
    "summary.csv",
    "eval_summary.json",
    "scenario_manifest.json",
    "latency_summary.json",
    "state_coverage.json",
    "annotated_decisions.txt",
    "annotated_decisions.ndjson",
};

struct BundledFile {
    std::string name;
    uintmax_t   size_bytes;
    uint64_t    fnv1a_hash;
    bool        present;
};

// ---- Bundle one run directory ----

int pack_run(const fs::path& run_dir, const fs::path& output_dir, bool list_only) {
    if (!fs::is_directory(run_dir)) {
        std::cerr << "[hermes_pack] Not a directory: " << run_dir << "\n";
        return 1;
    }

    // Read run_id from run_metadata.json if available.
    std::string run_id;
    {
        std::ifstream mf(run_dir / "run_metadata.json");
        if (mf.is_open()) {
            const std::string meta((std::istreambuf_iterator<char>(mf)),
                                    std::istreambuf_iterator<char>());
            run_id = jstr(meta, "run_id");
        }
    }
    if (run_id.empty()) run_id = run_dir.filename().string();

    fs::path dest = output_dir;
    if (dest.empty()) {
        dest = "artifacts/evidence_bundles/" + run_id;
    }

    if (list_only) {
        std::cout << "Run ID   : " << run_id << "\n";
        std::cout << "Source   : " << fs::absolute(run_dir).string() << "\n";
        std::cout << "Would bundle to: " << fs::absolute(dest).string() << "\n\n";
        for (const auto& name : ARTIFACT_FILES) {
            const fs::path src = run_dir / name;
            const bool exists = fs::exists(src);
            const uintmax_t sz = exists ? fs::file_size(src) : 0;
            std::cout << (exists ? "  [FOUND]  " : "  [absent] ")
                      << std::left << std::setw(40) << name;
            if (exists) std::cout << "  " << sz << " bytes";
            std::cout << "\n";
        }
        return 0;
    }

    // Create destination directory.
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        std::cerr << "[hermes_pack] Cannot create " << dest << ": " << ec.message() << "\n";
        return 1;
    }

    // Copy artifacts and build manifest.
    std::vector<BundledFile> manifest;
    int found = 0, missing = 0;

    for (const auto& name : ARTIFACT_FILES) {
        const fs::path src = run_dir / name;
        BundledFile bf;
        bf.name    = name;
        bf.present = fs::exists(src);

        if (bf.present) {
            bf.size_bytes  = fs::file_size(src);
            bf.fnv1a_hash  = fnv1a_file(src);
            fs::copy_file(src, dest / name, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[hermes_pack] Failed to copy " << name << ": " << ec.message() << "\n";
            } else {
                ++found;
            }
        } else {
            bf.size_bytes = 0;
            bf.fnv1a_hash = 0;
            ++missing;
        }
        manifest.push_back(bf);
    }

    // Write bundle_manifest.json.
    const fs::path manifest_path = dest / "bundle_manifest.json";
    {
        std::ofstream mf(manifest_path);
        if (!mf.is_open()) {
            std::cerr << "[hermes_pack] Cannot write manifest to " << manifest_path << "\n";
            return 1;
        }

        mf << "{\n";
        mf << "  \"run_id\": \"" << run_id << "\",\n";
        mf << "  \"bundled_at\": \"" << now_iso() << "\",\n";
        mf << "  \"source_dir\": \"" << run_dir.string() << "\",\n";
        mf << "  \"files_found\": " << found << ",\n";
        mf << "  \"files_missing\": " << missing << ",\n";
        mf << "  \"files\": [\n";

        bool first = true;
        for (const auto& bf : manifest) {
            if (!first) mf << ",\n";
            first = false;
            mf << "    {\"name\": \"" << bf.name << "\""
               << ", \"present\": " << (bf.present ? "true" : "false")
               << ", \"size_bytes\": " << bf.size_bytes
               << ", \"fnv1a_hex\": \"" << hex64(bf.fnv1a_hash) << "\"}";
        }
        mf << "\n  ]\n}\n";
    }

    std::cout << "[hermes_pack] Bundled run_id=" << run_id << "\n";
    std::cout << "  Source : " << fs::absolute(run_dir).string() << "\n";
    std::cout << "  Dest   : " << fs::absolute(dest).string() << "\n";
    std::cout << "  Files  : " << found << " copied, " << missing << " absent\n";
    std::cout << "  Manifest: " << manifest_path.string() << "\n";
    return (found == 0) ? 1 : 0;
}

bool has_arg(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a) if (x == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& a, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || has_arg(args, "--help")) {
        std::cout <<
            "Usage: hermes_pack <run-dir> [<run-dir> ...] [options]\n\n"
            "Packages a Hermes run directory into a portable evidence bundle.\n\n"
            "Options:\n"
            "  --output-dir <path>  Destination directory (default: artifacts/evidence_bundles/<run_id>/)\n"
            "  --list               List files that would be bundled without copying\n"
            "  --help               Show this help\n\n"
            "The bundle includes all NDJSON artifacts, metadata, config snapshot,\n"
            "replay summary, and a bundle_manifest.json with FNV-1a hashes.\n";
        return args.empty() ? 1 : 0;
    }

    const bool list_only  = has_arg(args, "--list");
    const std::string out = get_arg(args, "--output-dir", "");

    // Collect positional run directory arguments.
    std::vector<std::string> run_dirs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--output-dir") { ++i; continue; }
        if (args[i].empty() || args[i][0] == '-') continue;
        run_dirs.push_back(args[i]);
    }

    if (run_dirs.empty()) {
        std::cerr << "[hermes_pack] No run directory specified.\n";
        return 1;
    }

    int failures = 0;
    for (const auto& rd : run_dirs) {
        const fs::path output_dir = (run_dirs.size() == 1 && !out.empty())
            ? fs::path(out)
            : fs::path("");
        if (pack_run(fs::path(rd), output_dir, list_only) != 0) ++failures;
    }
    return failures > 0 ? 1 : 0;
}
