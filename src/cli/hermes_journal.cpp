// hermes_journal: human-readable Markdown timeline of a Hermes run.
//
// Reads a run directory and emits a chronological Markdown document showing
// UPS spikes, band transitions, scheduler state changes, interventions, and
// predictions in plain language.  Useful for post-mortem analysis, reports,
// and blog posts.
//
// Usage:
//   hermes_journal <run-dir>
//   hermes_journal <run-dir> --output report.md
//   hermes_journal <run-dir> --stdout
//   hermes_journal <run-dir> --no-samples  (omit raw sample rows)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool has_arg(int argc, char** argv, const std::string& f) {
    for (int i = 1; i < argc; ++i) if (f == argv[i]) return true;
    return false;
}
std::string get_arg(int argc, char** argv, const std::string& f, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) if (f == argv[i]) return argv[i + 1];
    return def;
}

double jdbl(const std::string& s, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = s.find(kk);
    if (p == std::string::npos) return 0.0;
    try { return std::stod(s.substr(p + kk.size())); } catch (...) { return 0.0; }
}
std::string jstr(const std::string& s, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = s.find(kk);
    if (p == std::string::npos) return "";
    const auto st = p + kk.size();
    const auto en = s.find('"', st);
    return en == std::string::npos ? "" : s.substr(st, en - st);
}
uint64_t jull(const std::string& s, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = s.find(kk);
    if (p == std::string::npos) return 0;
    try { return std::stoull(s.substr(p + kk.size())); } catch (...) { return 0; }
}

std::string ts_str(uint64_t wall_ms) {
    if (wall_ms == 0) return "—";
    const std::time_t t = static_cast<std::time_t>(wall_ms / 1000);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::gmtime(&t));
    return buf;
}

std::string band_emoji(const std::string& b) {
    if (b == "critical") return "🔴";
    if (b == "elevated") return "🟡";
    return "🟢";
}

std::string state_emoji(const std::string& s) {
    if (s == "Throttled")  return "⏸️";
    if (s == "Cooldown")   return "❄️";
    if (s == "Elevated")   return "⚠️";
    if (s == "Recovery")   return "🔄";
    return "✅";
}

// ---- Event types we collect ----

struct JournalEvent {
    uint64_t    ts_wall{0};   // ms since epoch
    uint64_t    ts_mono{0};   // ms monotonic (for ordering)
    std::string kind;         // "sample" | "score" | "prediction" | "decision" | "action" | "event"
    std::string summary;      // one-line human description
    bool        highlight{false};  // bold/notable
};

std::vector<JournalEvent> read_scores(const fs::path& dir) {
    std::vector<JournalEvent> out;
    std::ifstream f(dir / "scores.ndjson");
    if (!f.is_open()) return out;
    std::string line;
    std::string prev_band;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const double ups   = jdbl(line, "ups");
        const std::string band = jstr(line, "pressure_band");
        const uint64_t ts_w = jull(line, "ts_wall");
        const uint64_t ts_m = jull(line, "ts_mono");
        if (band != prev_band && !prev_band.empty()) {
            JournalEvent e;
            e.ts_wall = ts_w; e.ts_mono = ts_m;
            e.kind = "score";
            e.highlight = true;
            std::ostringstream ss;
            ss << band_emoji(band) << " **Band transition**: " << prev_band << " → " << band
               << " (UPS " << std::fixed << std::setprecision(1) << ups << ")";
            e.summary = ss.str();
            out.push_back(e);
        }
        prev_band = band;
    }
    return out;
}

std::vector<JournalEvent> read_decisions(const fs::path& dir) {
    std::vector<JournalEvent> out;
    std::ifstream f(dir / "decisions.ndjson");
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::string action = jstr(line, "action");
        const std::string level  = jstr(line, "level");
        const std::string state  = jstr(line, "scheduler_state");
        const double ups   = jdbl(line, "ups");
        const uint64_t ts_w = jull(line, "ts_wall");
        const uint64_t ts_m = jull(line, "ts_mono");
        if (action.empty() || action == "none") continue;
        JournalEvent e;
        e.ts_wall = ts_w; e.ts_mono = ts_m;
        e.kind = "decision"; e.highlight = true;
        std::ostringstream ss;
        ss << "🎯 **Decision** [" << level << "] action=`" << action << "`"
           << " state=" << state
           << " UPS=" << std::fixed << std::setprecision(1) << ups;
        e.summary = ss.str();
        out.push_back(e);
    }
    return out;
}

std::vector<JournalEvent> read_actions(const fs::path& dir) {
    std::vector<JournalEvent> out;
    std::ifstream f(dir / "actions.ndjson");
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::string atype  = jstr(line, "action_type");
        const std::string result = jstr(line, "result");
        const std::string pid_s  = jstr(line, "target_pid");
        const std::string pname  = jstr(line, "target_name");
        const uint64_t ts_w = jull(line, "ts_wall");
        const uint64_t ts_m = jull(line, "ts_mono");
        JournalEvent e;
        e.ts_wall = ts_w; e.ts_mono = ts_m;
        e.kind = "action"; e.highlight = true;
        std::ostringstream ss;
        ss << "⚡ **Action** `" << atype << "`";
        if (!pname.empty()) ss << " on **" << pname << "**";
        else if (!pid_s.empty()) ss << " on PID " << pid_s;
        ss << " → " << result;
        e.summary = ss.str();
        out.push_back(e);
    }
    return out;
}

std::vector<JournalEvent> read_events(const fs::path& dir) {
    std::vector<JournalEvent> out;
    std::ifstream f(dir / "events.ndjson");
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::string ekind = jstr(line, "kind");
        const uint64_t ts_w = jull(line, "ts_wall");
        const uint64_t ts_m = jull(line, "ts_mono");
        if (ekind == "band_transition") {
            const std::string from = jstr(line, "previous_band");
            const std::string to   = jstr(line, "new_band");
            const double ups = jdbl(line, "ups");
            JournalEvent e;
            e.ts_wall = ts_w; e.ts_mono = ts_m;
            e.kind = "event"; e.highlight = (to == "critical" || to == "elevated");
            std::ostringstream ss;
            ss << band_emoji(to) << " **Band event**: " << from << " → " << to
               << " (UPS " << std::fixed << std::setprecision(1) << ups << ")";
            e.summary = ss.str();
            out.push_back(e);
        } else if (ekind == "state_transition") {
            const std::string from = jstr(line, "previous_state");
            const std::string to   = jstr(line, "new_state");
            JournalEvent e;
            e.ts_wall = ts_w; e.ts_mono = ts_m;
            e.kind = "event"; e.highlight = true;
            std::ostringstream ss;
            ss << state_emoji(to) << " **State**: " << from << " → **" << to << "**";
            e.summary = ss.str();
            out.push_back(e);
        }
    }
    return out;
}

std::vector<JournalEvent> read_predictions(const fs::path& dir) {
    std::vector<JournalEvent> out;
    std::ifstream f(dir / "predictions.ndjson");
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const double risk = jdbl(line, "risk_score");
        const std::string band = jstr(line, "risk_band");
        if (band != "high" && band != "critical") continue;
        const std::string reason = jstr(line, "reason_code");
        const uint64_t ts_w = jull(line, "ts_wall");
        const uint64_t ts_m = jull(line, "ts_mono");
        JournalEvent e;
        e.ts_wall = ts_w; e.ts_mono = ts_m;
        e.kind = "prediction"; e.highlight = true;
        std::ostringstream ss;
        ss << "🔮 **High-risk prediction**: risk=" << std::fixed << std::setprecision(3) << risk
           << " [" << band << "] reason=`" << reason << "`";
        e.summary = ss.str();
        out.push_back(e);
    }
    return out;
}

// ---- Run metadata ----

struct RunMeta {
    std::string run_id;
    std::string scenario;
    std::string host;
    uint64_t    first_ts{0};
    uint64_t    last_ts{0};
    double      peak_ups{0.0};
    std::size_t sample_count{0};
};

RunMeta read_meta(const fs::path& dir) {
    RunMeta m;
    std::ifstream f(dir / "run_metadata.json");
    if (!f.is_open()) { m.run_id = dir.filename().string(); return m; }
    const std::string j((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    m.run_id   = jstr(j, "run_id");
    m.scenario = jstr(j, "scenario");
    m.host     = jstr(j, "hostname");
    if (m.run_id.empty()) m.run_id = dir.filename().string();

    // Read peak_ups from telemetry_quality.json.
    std::ifstream tq(dir / "telemetry_quality.json");
    if (tq.is_open()) {
        const std::string tqj((std::istreambuf_iterator<char>(tq)),
                               std::istreambuf_iterator<char>());
        m.peak_ups = jdbl(tqj, "peak_ups");
    }

    // Count samples.
    std::ifstream sf(dir / "samples.ndjson");
    if (sf.is_open()) {
        std::string l;
        while (std::getline(sf, l)) if (!l.empty()) ++m.sample_count;
    }
    return m;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::cout <<
            "Usage: hermes_journal <run-dir> [options]\n\n"
            "Generates a human-readable Markdown timeline of a Hermes run.\n\n"
            "Options:\n"
            "  --output <file>   Write to file instead of run-dir/journal.md\n"
            "  --stdout          Print to stdout instead of writing a file\n"
            "  --no-samples      Omit raw sample rows (only show events)\n";
        return argc < 2 ? 1 : 0;
    }

    const fs::path run_dir  = argv[1];
    const bool to_stdout    = has_arg(argc, argv, "--stdout");
    const std::string out_f = get_arg(argc, argv, "--output", "");

    if (!fs::is_directory(run_dir)) {
        std::cerr << "hermes_journal: not a directory: " << run_dir << "\n";
        return 1;
    }

    // Collect events from all artifact files.
    std::vector<JournalEvent> events;
    auto append = [&](std::vector<JournalEvent> v) {
        events.insert(events.end(), v.begin(), v.end());
    };
    append(read_scores(run_dir));
    append(read_events(run_dir));
    append(read_predictions(run_dir));
    append(read_decisions(run_dir));
    append(read_actions(run_dir));

    // Sort by monotonic timestamp (wall may drift).
    std::sort(events.begin(), events.end(), [](const JournalEvent& a, const JournalEvent& b) {
        return a.ts_mono < b.ts_mono;
    });

    const RunMeta meta = read_meta(run_dir);

    // ---- Render Markdown ----
    std::ostringstream md;

    md << "# Hermes Run Journal\n\n";
    md << "| Field | Value |\n| --- | --- |\n";
    md << "| Run ID | `" << meta.run_id << "` |\n";
    md << "| Scenario | " << (meta.scenario.empty() ? "—" : meta.scenario) << " |\n";
    md << "| Host | " << (meta.host.empty() ? "—" : meta.host) << " |\n";
    md << "| Samples | " << meta.sample_count << " |\n";
    md << "| Peak UPS | " << std::fixed << std::setprecision(1) << meta.peak_ups << " |\n";
    md << "| Notable events | " << events.size() << " |\n\n";

    md << "---\n\n";
    md << "## Timeline\n\n";

    if (events.empty()) {
        md << "_No notable events found. Run produced no band transitions, predictions, decisions, or actions._\n";
    } else {
        md << "| Time (UTC) | Event |\n| --- | --- |\n";
        for (const auto& e : events) {
            md << "| " << ts_str(e.ts_wall) << " | " << e.summary << " |\n";
        }
    }

    md << "\n---\n\n";
    md << "## Artifact Inventory\n\n";

    const std::vector<std::string> known = {
        "run_metadata.json", "config_snapshot.yaml", "telemetry_quality.json",
        "samples.ndjson", "scores.ndjson", "predictions.ndjson",
        "decisions.ndjson", "actions.ndjson", "events.ndjson", "processes.ndjson",
        "replay_summary.json", "eval_summary.json", "scenario_manifest.json",
    };
    md << "| Artifact | Present | Size |\n| --- | --- | --- |\n";
    for (const auto& name : known) {
        const fs::path p = run_dir / name;
        const bool exists = fs::exists(p);
        const uintmax_t sz = exists ? fs::file_size(p) : 0;
        md << "| `" << name << "` | " << (exists ? "✅" : "❌") << " | ";
        if (exists) md << sz << " B";
        md << " |\n";
    }

    md << "\n---\n\n";
    md << "_Generated by `hermes_journal`. Source: `" << fs::absolute(run_dir).string() << "`_\n";

    // ---- Output ----
    if (to_stdout) {
        std::cout << md.str();
        return 0;
    }

    const fs::path dest = out_f.empty() ? (run_dir / "journal.md") : fs::path(out_f);
    std::ofstream of(dest);
    if (!of.is_open()) {
        std::cerr << "hermes_journal: cannot write " << dest << "\n";
        return 1;
    }
    of << md.str();
    std::cout << "Wrote " << dest.string() << "\n";
    return 0;
}
