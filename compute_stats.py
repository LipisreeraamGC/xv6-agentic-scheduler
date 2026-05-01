#!/usr/bin/env python3
"""
Tier 2 statistical analysis for eval_results.md.

Reads:
  eval_results.json       -- bench_cpu and bench_io (combined session)
  eval_mixed_results.json -- bench_mixed (isolated sessions, clean data)

Computes per (config, workload):
  - Median, min, max turnaround
  - 95% bootstrap CI for median (n=5, 10000 resamples)
  - Mann-Whitney U: each config vs RR baseline (turnaround)

Prints a Markdown table ready to paste into eval_results.md.
"""

import json
import random
import os
from scipy.stats import mannwhitneyu

SCRIPT_DIR = os.path.dirname(__file__)


def load_data():
    """Return dict: (config, workload) -> list of turnaround ints."""
    data = {}

    with open(os.path.join(SCRIPT_DIR, "eval_results.json")) as f:
        raw = json.load(f)

    cfg_map = {
        "config_rr":          "rr",
        "config_mlfq":        "mlfq",
        "config_mlfq_sagent": "agent",
    }

    for cfg_raw, benches in raw.items():
        cfg = cfg_map[cfg_raw]
        for bench, runs in benches.items():
            if bench == "bench_mixed":
                continue  # use isolated data below
            vals = [r["turnaround"] for r in runs if "turnaround" in r]
            # Remove the 76-tick RR bench_cpu outlier
            if cfg == "rr" and bench == "bench_cpu":
                vals = [v for v in vals if v < 70]
            data[(cfg, bench)] = vals

    with open(os.path.join(SCRIPT_DIR, "eval_mixed_results.json")) as f:
        mixed_raw = json.load(f)

    for cfg_raw, runs in mixed_raw.items():
        cfg = cfg_map[cfg_raw]
        vals = [r["turnaround"] for r in runs if "turnaround" in r]
        data[(cfg, "bench_mixed")] = vals

    return data


def median(vals):
    s = sorted(vals)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def bootstrap_ci(vals, n_boot=10000, ci=0.95, seed=42):
    """Bootstrap CI for the median."""
    random.seed(seed)
    n = len(vals)
    boot_medians = []
    for _ in range(n_boot):
        sample = [vals[random.randint(0, n - 1)] for _ in range(n)]
        boot_medians.append(median(sample))
    boot_medians.sort()
    lo = boot_medians[int((1 - ci) / 2 * n_boot)]
    hi = boot_medians[int((1 + ci) / 2 * n_boot)]
    return lo, hi


def mw_pvalue(a, b):
    """Mann-Whitney U p-value (two-sided). Returns 'n/a' if all values equal."""
    if a == b or (len(set(a)) == 1 and len(set(b)) == 1 and set(a) == set(b)):
        return "n/a"
    try:
        _, p = mannwhitneyu(a, b, alternative="two-sided")
        return f"{p:.3f}"
    except Exception:
        return "n/a"


def improvement_pct(rr_med, cfg_med):
    if rr_med == 0:
        return "n/a"
    return f"{(rr_med - cfg_med) / rr_med * 100:+.1f}%"


def main():
    data = load_data()
    benches = ["bench_cpu", "bench_io", "bench_mixed"]
    configs = ["rr", "mlfq", "agent"]

    print("## Statistical Analysis (Bootstrap CI + Mann-Whitney)\n")
    print("> n=5 per cell; 95% bootstrap CI (10 000 resamples); ")
    print("> Mann-Whitney U p-value vs RR baseline (two-sided).\n")

    # Per-workload tables
    for bench in benches:
        print(f"### {bench}\n")
        print(f"| config | median | 95% CI           | min | max | vs RR Δ%  | MW p vs RR |")
        print(f"|--------|-------:|------------------|----:|----:|----------:|------------|")

        rr_vals = data.get(("rr", bench), [])
        rr_med  = median(rr_vals)

        for cfg in configs:
            vals = data.get((cfg, bench), [])
            if not vals:
                print(f"| {cfg:<6} | —      | —                |   — |   — | —         | —          |")
                continue
            med       = median(vals)
            lo, hi    = bootstrap_ci(vals)
            mn, mx    = min(vals), max(vals)
            impr      = improvement_pct(rr_med, med) if cfg != "rr" else "baseline"
            p_val     = mw_pvalue(rr_vals, vals) if cfg != "rr" else "—"
            ci_str    = f"[{lo:.0f}, {hi:.0f}]"
            print(f"| {cfg:<6} | {med:>6.1f} | {ci_str:<16} | {mn:>3} | {mx:>3} | {impr:>9} | {p_val:<10} |")
        print()

    # Summary: improvement table
    print("### Improvement summary (median turnaround vs RR)\n")
    print("| workload    | mlfq Δ%   | agent Δ%  | mlfq MW-p | agent MW-p |")
    print("|-------------|----------:|----------:|----------:|------------|")
    for bench in benches:
        rr_vals   = data.get(("rr", bench), [])
        rr_med    = median(rr_vals)
        mlfq_vals = data.get(("mlfq", bench), [])
        agent_vals = data.get(("agent", bench), [])

        mlfq_d  = improvement_pct(rr_med, median(mlfq_vals))
        agent_d = improvement_pct(rr_med, median(agent_vals))
        mlfq_p  = mw_pvalue(rr_vals, mlfq_vals)
        agent_p = mw_pvalue(rr_vals, agent_vals)
        print(f"| {bench:<11} | {mlfq_d:>9} | {agent_d:>9} | {mlfq_p:>9} | {agent_p:<10} |")


if __name__ == "__main__":
    main()
