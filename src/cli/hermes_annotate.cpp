// hermes_annotate: annotates decisions.ndjson with human-readable rationale.
//
// Reads decisions.ndjson + scores.ndjson + predictions.ndjson from a run
// directory and writes annotated_decisions.ndjson where every record has an
// added "annotation" field explaining in plain English WHY Hermes made the
// decision and WHAT the expected outcome is.
//
// Also writes annotated_decisions.txt — a plain-text audit log suitable for
// human review or attaching as a defensibility artifact.
//
// Usage:
//   hermes_annotate <run-dir>
//   hermes_annotate <run-dir> --out <output-dir>
//   hermes_annotate <run-dir> --txt-only

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string jstr(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = j.find(kk);
    if (p == std::string::npos) return "";
    const auto s = p + kk.size(), e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
double jdbl(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return -1.0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return -1.0;
    try { return std::stod(j.substr(s)); } catch (...) { return -1.0; }
}
uint64_t jull(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return 0;
    try { return std::stoull(j.substr(s)); } catch (...) { return 0; }
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

// Map action kind to plain-English verb phrase.
std::string action_phrase(const std::string& action, const std::string& level) {
    if (action == "reprioritize" || level == "level1")
        return "lowered the scheduling priority of a background process (Level 1: reprioritize)";
    if (action == "throttle" || level == "level2")
        return "suspended a background process via SIGSTOP (Level 2: throttle)";
    if (action == "terminate_candidate" || level == "level3")
        return "terminated a background process via SIGTERM/SIGKILL (Level 3: kill)";
    if (action == "observe" || action == "")
        return "took no action (observe-only mode or no eligible candidate)";
    return "executed action: " + action;
}

// Map cooldown_state to plain-English explanation.
std::string cooldown_phrase(const std::string& cs) {
    if (cs == "global-level3") return "global Level-3 cooldown is active — suppressing further interventions";
    if (cs == "pid-level2")    return "per-PID Level-2 cooldown active — same process was recently throttled";
    if (cs == "pid-level1")    return "per-PID Level-1 cooldown active — same process was recently reprioritized";
    if (cs == "circuit-breaker") return "circuit breaker tripped — too many interventions in the last 60 s";
    if (cs == "clear" || cs.empty()) return "";
    return "cooldown: " + cs;
}

// Build the annotation string for one decision record.
std::string annotate(const std::string& dec_line,
                     const std::string& score_line,
                     const std::string& pred_line,
                     uint64_t frame_num) {
    const std::string level   = jstr(dec_line, "level");
    const std::string action  = jstr(dec_line, "action");
    const std::string state   = jstr(dec_line, "scheduler_state");
    const std::string prev    = jstr(dec_line, "previous_scheduler_state");
    const std::string cs      = jstr(dec_line, "cooldown_state");
    const std::string why     = jstr(dec_line, "why");
    const bool should_exec    = dec_line.find("\"should_execute\":true") != std::string::npos;

    const double ups      = jdbl(score_line, "ups");
    const std::string band = jstr(score_line, "pressure_band");
    const double risk     = jdbl(pred_line, "risk_score");
    const std::string rband = jstr(pred_line, "risk_band");
    const std::string pred_event = jstr(pred_line, "predicted_event");
    const double lead_s   = jdbl(pred_line, "lead_time_s");

    std::ostringstream ann;
    ann << std::fixed << std::setprecision(2);

    ann << "Frame " << frame_num << ": ";

    // Pressure summary.
    if (ups >= 0.0)
        ann << "UPS=" << ups << " (" << band << "), ";
    if (risk >= 0.0)
        ann << "risk=" << risk << " (" << rband << ")";
    if (!pred_event.empty() && pred_event != "none")
        ann << ", predicted=" << pred_event;
    if (lead_s > 0.0)
        ann << " in " << std::setprecision(1) << lead_s << "s";
    ann << ". ";

    // State transition.
    if (!prev.empty() && prev != state)
        ann << "Scheduler: " << prev << " → " << state << ". ";
    else if (!state.empty())
        ann << "Scheduler state: " << state << ". ";

    // Action taken.
    ann << "Hermes " << action_phrase(action, level) << ". ";

    // Cooldown context.
    const std::string cp = cooldown_phrase(cs);
    if (!cp.empty()) ann << "[" << cp << "] ";

    // Execution mode.
    if (!should_exec)
        ann << "(dry-run — no system mutation)";
    else
        ann << "(active-control — mutation applied)";

    // Root cause from why field.
    if (!why.empty()) ann << " Reason: " << why << ".";

    return ann.str();
}

// Escape a string for JSON embedding.
std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || has_arg(args, "--help")) {
        std::cout << "Usage: hermes_annotate <run-dir> [--out <dir>] [--txt-only]\n";
        return 0;
    }

    const std::string run_dir = args[0];
    const std::string out_dir = get_arg(args, "--out", run_dir);
    const bool txt_only = has_arg(args, "--txt-only");

    // Open input files.
    std::ifstream f_dec  (run_dir + "/decisions.ndjson");
    std::ifstream f_score(run_dir + "/scores.ndjson");
    std::ifstream f_pred (run_dir + "/predictions.ndjson");

    if (!f_dec.is_open()) {
        std::cerr << "hermes_annotate: cannot open " << run_dir << "/decisions.ndjson\n";
        return 1;
    }

    // Open outputs.
    std::ofstream f_ann_ndjson;
    std::ofstream f_ann_txt(out_dir + "/annotated_decisions.txt");
    if (!txt_only)
        f_ann_ndjson.open(out_dir + "/annotated_decisions.ndjson");

    f_ann_txt << "Hermes Decision Audit Log\n";
    f_ann_txt << "Run directory: " << run_dir << "\n";
    f_ann_txt << std::string(70, '=') << "\n\n";

    uint64_t frame = 0;
    uint64_t total = 0, interventions = 0;
    std::string dec_line, score_line, pred_line;

    while (std::getline(f_dec, dec_line)) {
        if (dec_line.empty()) continue;
        ++frame;

        score_line.clear();
        pred_line.clear();
        if (f_score.is_open()) std::getline(f_score, score_line);
        if (f_pred.is_open())  std::getline(f_pred,  pred_line);

        const std::string ann = annotate(dec_line, score_line, pred_line, frame);
        const std::string action = jstr(dec_line, "action");
        if (action != "observe" && !action.empty()) ++interventions;
        ++total;

        // Write annotated NDJSON: original record + "annotation" field appended.
        if (f_ann_ndjson.is_open()) {
            // Strip trailing } and append annotation field.
            std::string rec = dec_line;
            const auto last_brace = rec.rfind('}');
            if (last_brace != std::string::npos)
                rec = rec.substr(0, last_brace) + ",\"annotation\":\"" + json_escape(ann) + "\"}";
            f_ann_ndjson << rec << "\n";
        }

        // Write plain-text audit entry.
        f_ann_txt << ann << "\n";
    }

    f_ann_txt << "\n" << std::string(70, '=') << "\n";
    f_ann_txt << "Summary: " << total << " frames, " << interventions << " interventions.\n";

    std::cout << "hermes_annotate complete\n";
    std::cout << "  Frames        : " << total << "\n";
    std::cout << "  Interventions : " << interventions << "\n";
    std::cout << "  Output dir    : " << out_dir << "\n";
    if (!txt_only)
        std::cout << "  NDJSON        : " << out_dir << "/annotated_decisions.ndjson\n";
    std::cout << "  Audit log     : " << out_dir << "/annotated_decisions.txt\n";
    return 0;
}
