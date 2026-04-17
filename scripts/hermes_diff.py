#!/usr/bin/env python3
"""hermes_diff.py — side-by-side schema YAML diff with impact estimates.

Compares two Hermes schema YAML files and prints:
  - Changed threshold values with a direction indicator (tighter/looser)
  - Changed UPS weights with impact estimate on steady-state UPS
  - Changed action flags and cooldown values
  - New or removed top-level keys

Usage:
    python3 scripts/hermes_diff.py config/schema_tier_b.yaml config/schema_tier_c.yaml
    python3 scripts/hermes_diff.py config/schema.yaml config/schema_tier_c.yaml --show-unchanged
    python3 scripts/hermes_diff.py A.yaml B.yaml --json   # machine-readable JSON output
"""

import argparse
import json
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Minimal YAML leaf-value parser (no PyYAML dependency)
# ---------------------------------------------------------------------------

def parse_yaml_flat(path: str) -> dict:
    """Return a flat dict of dotted-key → scalar-value from a simple YAML file."""
    result = {}
    section_stack = []   # list of (indent, key) pairs

    with open(path, encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.rstrip()
            if not line or line.lstrip().startswith("#"):
                continue

            # Measure indent.
            indent = len(line) - len(line.lstrip(" \t"))

            # Strip inline comment.
            comment_pos = line.find(" #")
            if comment_pos > 0:
                line = line[:comment_pos]
            line = line.strip()
            if not line or ":" not in line:
                continue

            colon = line.index(":")
            key   = line[:colon].strip()
            value = line[colon + 1:].strip()

            # Pop stack to current indent level.
            while section_stack and section_stack[-1][0] >= indent:
                section_stack.pop()

            if value == "" or value == "[]":
                # Section header or list placeholder.
                section_stack.append((indent, key))
                if value == "[]":
                    dotted = ".".join(s[1] for s in section_stack)
                    result[dotted] = "[]"
                    section_stack.pop()
            else:
                section_stack.append((indent, key))
                dotted = ".".join(s[1] for s in section_stack)
                # Normalise boolean strings.
                if value.lower() == "true":
                    result[dotted] = True
                elif value.lower() == "false":
                    result[dotted] = False
                else:
                    try:
                        result[dotted] = float(value) if "." in value else int(value)
                    except ValueError:
                        result[dotted] = value
                section_stack.pop()

    return result


# ---------------------------------------------------------------------------
# Impact estimation helpers
# ---------------------------------------------------------------------------

# Impact of a UPS weight change: delta_weight * 100 at full signal = UPS pts.
def weight_impact(old_w, new_w):
    delta = new_w - old_w
    impact = delta * 100  # worst-case UPS shift (signal = 1.0)
    direction = "raises UPS" if impact > 0 else "lowers UPS"
    return f"{direction} ≤{abs(impact):.1f} pts at full signal"


# Impact of a threshold change.
def threshold_impact(key, old_v, new_v):
    delta = new_v - old_v
    if delta < 0:
        return "tighter → triggers earlier"
    return "looser → triggers later"


# Impact of a cooldown change.
def cooldown_impact(old_v, new_v):
    delta = new_v - old_v
    if delta > 0:
        return f"longer cooldown (+{delta}s) → less aggressive"
    return f"shorter cooldown ({delta}s) → more aggressive"


# ---------------------------------------------------------------------------
# Diff logic
# ---------------------------------------------------------------------------

CATEGORY_RULES = [
    ("ups_weights",   "UPS Weight",    weight_impact),
    ("thresholds",    "Threshold",     threshold_impact),
    ("cooldowns",     "Cooldown",      cooldown_impact),
    ("actions",       "Action flag",   None),
    ("circuit_breaker", "Circuit breaker", None),
    ("multi_gpu",     "Multi-GPU",     None),
    ("protected",     "Protection",    None),
]

def categorise(key):
    for prefix, label, fn in CATEGORY_RULES:
        if key.startswith(prefix):
            return label, fn
    return "Other", None


def diff_schemas(a: dict, b: dict, show_unchanged: bool):
    all_keys = sorted(set(a) | set(b))
    rows = []

    for key in all_keys:
        in_a = key in a
        in_b = key in b

        if in_a and in_b:
            va, vb = a[key], b[key]
            changed = (va != vb)
            if not changed and not show_unchanged:
                continue
            cat, impact_fn = categorise(key)
            impact = ""
            if changed and impact_fn and isinstance(va, (int, float)) and isinstance(vb, (int, float)):
                try:
                    impact = impact_fn(key, va, vb)
                except Exception:
                    pass
            rows.append({
                "key": key, "category": cat,
                "a": va, "b": vb,
                "changed": changed,
                "status": "changed" if changed else "same",
                "impact": impact,
            })
        elif in_a:
            rows.append({"key": key, "category": categorise(key)[0],
                         "a": a[key], "b": "—", "changed": True,
                         "status": "removed", "impact": ""})
        else:
            rows.append({"key": key, "category": categorise(key)[0],
                         "a": "—", "b": b[key], "changed": True,
                         "status": "added", "impact": ""})

    return rows


# ---------------------------------------------------------------------------
# Renderers
# ---------------------------------------------------------------------------

def render_table(rows, path_a, path_b):
    w_key = max(36, max((len(r["key"]) for r in rows), default=20))
    w_val = 14
    w_imp = 40

    sep = "=" * (w_key + w_val * 2 + w_imp + 20)
    print(sep)
    print(f"hermes_diff — schema comparison")
    print(f"  A: {path_a}")
    print(f"  B: {path_b}")
    print(sep)
    print(f"{'Key':<{w_key}}  {'A':>{w_val}}  {'B':>{w_val}}  {'Status':<10}  Impact")
    print("-" * len(sep))

    last_cat = None
    changed_count = 0

    for r in rows:
        cat = r["category"]
        if cat != last_cat:
            if last_cat is not None:
                print()
            print(f"  [{cat}]")
            last_cat = cat

        a_str = str(r["a"])
        b_str = str(r["b"])
        status = r["status"]
        marker = " " if not r["changed"] else ("+" if status == "added" else
                                                "-" if status == "removed" else "~")
        impact = r["impact"][:w_imp - 3] + "..." if len(r["impact"]) > w_imp else r["impact"]

        key_str = r["key"]
        if len(key_str) > w_key:
            key_str = key_str[:w_key - 3] + "..."

        print(f"{marker} {key_str:<{w_key - 2}}  {a_str:>{w_val}}  {b_str:>{w_val}}  {status:<10}  {impact}")
        if r["changed"]:
            changed_count += 1

    print(sep)
    print(f"  {changed_count} changed, {len(rows) - changed_count} unchanged")
    print()
    print("Legend:")
    print("  ~  changed value     +  key added in B     -  key removed from B")
    print("  Impact column shows estimated effect on UPS or intervention timing.")


def render_json(rows, path_a, path_b):
    out = {
        "schema_a": path_a,
        "schema_b": path_b,
        "total_keys": len(rows),
        "changed_keys": sum(1 for r in rows if r["changed"]),
        "diffs": rows,
    }
    print(json.dumps(out, indent=2, default=str))


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Side-by-side diff of two Hermes schema YAML files.")
    parser.add_argument("schema_a", help="First schema YAML (baseline)")
    parser.add_argument("schema_b", help="Second schema YAML (comparison)")
    parser.add_argument("--show-unchanged", action="store_true",
                        help="Also print keys whose values are identical")
    parser.add_argument("--json", action="store_true",
                        help="Output machine-readable JSON instead of a table")
    args = parser.parse_args()

    if not Path(args.schema_a).exists():
        sys.exit(f"hermes_diff: cannot find {args.schema_a}")
    if not Path(args.schema_b).exists():
        sys.exit(f"hermes_diff: cannot find {args.schema_b}")

    a = parse_yaml_flat(args.schema_a)
    b = parse_yaml_flat(args.schema_b)
    rows = diff_schemas(a, b, show_unchanged=args.show_unchanged)

    if args.json:
        render_json(rows, args.schema_a, args.schema_b)
    else:
        render_table(rows, args.schema_a, args.schema_b)


if __name__ == "__main__":
    main()
