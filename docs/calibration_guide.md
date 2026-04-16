# Hermes Predictor Calibration Guide

This guide walks through the complete predictor calibration cycle for Hermes.
The goal is to reach the targets defined in `design.md`:

| Metric | Target |
| --- | --- |
| Precision | ≥ 0.85 |
| Recall | ≥ 0.80 |
| F1 | ≥ 0.80 |
| Mean lead time | ≥ 3.0 s |
| False positive rate | < 5 / hr |

**Prerequisites**: native Linux or WSL2 host with PSI support, `stress-ng`, and a CMake build of Hermes.

---

## Step 0 — Verify host readiness

```bash
bash scripts/hermes_doctor.sh --build-dir build
```

All items should be PASS or WARN. No FAIL items. If PSI is missing (`/proc/pressure/cpu` not found), the calibration cannot proceed.

---

## Step 1 — Run a high-pressure baseline to collect raw predictions

Run the observe-only scenario with fidelity workloads. Hermes logs every predictor decision to `predictions.ndjson` alongside real `events.ndjson`.

```bash
hermes_bench config/observe_scenario.yaml \
    --run-id calibration-baseline-001 \
    --hermes-bin build/hermesd \
    --replay-bin build/hermes_replay
```

After the run, find the hermes run directory:

```bash
ls artifacts/logs/ | grep calibration-baseline-001
# e.g.: calibration-baseline-001-hermes
```

---

## Step 2 — Evaluate predictor quality

Run `hermes_eval` on the run directory. It reads `predictions.ndjson` and `events.ndjson` and writes `eval_summary.json`.

```bash
build/hermes_eval artifacts/logs/calibration-baseline-001-hermes
```

Stdout will show the metrics table. The JSON is also written to the run directory.

---

## Step 3 — Check calibration targets

Run `hermes_tune.py` to compare the eval results against calibration targets and get specific adjustment suggestions:

```bash
python3 scripts/hermes_tune.py --eval-dir artifacts/logs \
    --schema config/schema.yaml
```

Output example:

```
================================================================
Hermes Predictor Calibration Report  (1 eval run(s))
================================================================
  Metric                               Value      Target                          Status
  ----------------------------------- ----------  ------------------------------  ------
  precision                               0.723   precision >= 0.85               FAIL
  recall                                  0.811   recall >= 0.80                  PASS
  false_positive_rate_per_hour            7.200   FP rate < 5/hr                  FAIL
  mean_lead_time_s                        4.100   mean lead time >= 3s            PASS
  f1                                      0.763   F1 >= 0.80                      FAIL

Suggested config/schema.yaml adjustments:
  ups_critical_threshold: raise by 5 (critical band too broad)
  vram_slope_fast_threshold: raise (fast-slope window too sensitive)
  ups_elevated_threshold: raise by 5-10 (currently fires too easily)
  risk_high_threshold:    raise by 0.05 (high-risk band too wide)
```

---

## Step 4 — Apply threshold adjustments

Edit `config/schema.yaml` using the suggestions. The key thresholds are:

### `thresholds.ups.elevated` (default: 40)

Controls when Hermes enters `ELEVATED` state. Raising it reduces false alarms on lightly-loaded hosts. Lowering it gives more lead time.

**Rule of thumb**: if `false_positive_rate_per_hour > 5`, raise by 5–10. If `mean_lead_time_s < 3`, lower by 5.

```yaml
thresholds:
  ups:
    elevated: 45    # raised from 40 — was firing too early on this host
    critical: 70
```

### `thresholds.ups.critical` (default: 70)

Controls when Hermes escalates to `CRITICAL` and considers L2/L3 actions. Raising it reduces aggressive interventions.

**Rule of thumb**: if `precision < 0.85`, raise by 5.

### `thresholds.predictor.high_risk` (default: 0.70)

The minimum risk score to classify a prediction as "high risk" and count it as a positive. Raising it improves precision at the cost of recall.

**Rule of thumb**: if `precision < 0.85` after UPS tuning, raise by 0.05 (max: 0.90). If `recall < 0.80`, lower by 0.05 (min: 0.50).

```yaml
thresholds:
  predictor:
    high_risk: 0.75    # raised from 0.70
```

### `thresholds.vram.high_pct` (default: 90)

VRAM utilisation percentage above which VRAM pressure contributes to risk score. Lowering it makes the predictor sensitive to smaller VRAM margins.

### `cooldowns.level_1_pid_sec` (default: 15)

Minimum seconds between repeated L1 actions on the same PID. Increasing this prevents excessive re-firing that inflates `false_positive_rate_per_hour`.

---

## Step 5 — Verify synthetic fixture still passes

After any threshold change, run the synthetic replay to confirm the 17/17 baseline assertions still pass. This guards against over-tuning that breaks the expected signal paths.

```bash
# Build and run synthetic fixture
build/hermes_synth --pressure artifacts/logs/synth-verify --run-id synth-verify
build/hermes_replay artifacts/logs/synth-verify

# Check assertion results
cat artifacts/replay/synth-verify-summary.json | python3 -c "
import json, sys
d = json.load(sys.stdin)
print(f'assertions: {d.get(\"assertions_passed\",0)}/{d.get(\"assertions_total\",0)}')
print('PASS' if d.get('assertions_failed',1)==0 else 'FAIL')
"
```

If assertions fail, your threshold change broke a signal path. Either revert or re-tune more carefully.

---

## Step 6 — Re-run eval and iterate

Repeat Steps 1–5 until `hermes_tune.py` reports all targets as PASS:

```
All calibration targets met. No threshold changes needed.
```

Typical convergence takes 2–4 iterations.

---

## Step 7 — Run the false-positive baseline check

With the tuned schema, verify Hermes does not fire on a quiet host:

```bash
hermes_bench config/low_pressure_scenario.yaml \
    --run-id fp-check-001 \
    --hermes-bin build/hermesd \
    --replay-bin build/hermes_replay \
    --verify-targets
```

Expected: exits 0, `intervention_count = 0` or 1.

If it fails, the `ups_elevated_threshold` is too low. Raise it by 5 and re-run.

---

## Step 8 — Record results in RESULTS.md

Once all targets pass, add the calibration evidence to `RESULTS.md`:

```markdown
| Predictor precision ≥ 0.85, recall ≥ 0.80, FP rate < 5/hr | T2 | **proven** |
  `artifacts/logs/<run>/eval_summary.json` | precision=0.87 recall=0.82 FP/hr=3.1 |
```

---

## Reference: threshold sensitivity table

| Threshold | Increasing | Decreasing |
| --- | --- | --- |
| `ups.elevated` | Fewer alarms, later detection | More alarms, earlier detection |
| `ups.critical` | Fewer L2/L3 actions | More aggressive interventions |
| `predictor.high_risk` | Higher precision, lower recall | Lower precision, higher recall |
| `vram.high_pct` | Less sensitive to VRAM fill | Earlier VRAM pressure signal |
| `cooldowns.level_1_pid_sec` | Lower re-fire rate | More frequent L1 action |

---

## Automation: run full Phase 6 calibration in one step

```bash
bash scripts/smoke_phase6.sh
```

This automates Phase 6a–d: PSI validation → fidelity workload + eval → hermes_tune.py check → low-pressure FP check.
