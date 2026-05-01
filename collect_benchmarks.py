#!/usr/bin/env python3
"""
collect_benchmarks.py
---------------------
Automatically runs bench_mixed N times for each scheduler config
(round-robin, mlfq-only, mlfq+agent) and saves raw results to
bench_results.json for later plotting.

Usage:
    python3 collect_benchmarks.py              # 15 runs per config (default)
    python3 collect_benchmarks.py --runs 20    # 20 runs per config
    python3 collect_benchmarks.py --runs 5     # quick smoke-test

Requirements:
    pip3 install pexpect
"""

import pexpect
import json
import re
import time
import os
import sys
import subprocess
import argparse
from datetime import datetime

# ── Config ────────────────────────────────────────────────────────────────────
XV6_PATH   = os.path.expanduser("~/Desktop/Notes/OS/FTP/xv6-public")
RUNS       = 15         # runs per config (override with --runs N)
TIMEOUT    = 600        # seconds to wait for each bench_mixed to finish
BOOT_WAIT  = 30         # seconds to wait for xv6 to boot
OUTPUT     = "bench_results.json"

CONFIGS = [
    {
        "name": "rr",
        "label": "Round-Robin",
        "setup": ["config_rr"],
        "pre_bench": [],
        "warmup_runs": 0,
    },
    {
        "name": "mlfq",
        "label": "Static MLFQ+SJF",
        "setup": ["config_mlfq"],
        "pre_bench": [],
        "warmup_runs": 0,
    },
    {
        "name": "agent",
        "label": "MLFQ+SJF + Agent",
        "setup": ["config_mlfq"],
        "pre_bench": ["sagent &"],
        "warmup_runs": 3,
    },
]

PROMPT = r"\$\s"   # matches "$ " prompt in xv6


def log(msg):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def boot_xv6():
    """Spawn QEMU and wait for first xv6 shell prompt."""
    log("Booting xv6...")
    child = pexpect.spawn(
        "bash",
        ["-c", "make qemu-nox 2>&1"],
        cwd=XV6_PATH,
        encoding="utf-8",
        timeout=BOOT_WAIT,
        echo=False,
    )
    child.logfile_read = sys.stdout   # show output in terminal
    try:
        child.expect(PROMPT, timeout=BOOT_WAIT)
        log("xv6 booted successfully.")
        return child
    except pexpect.TIMEOUT:
        log("ERROR: xv6 did not boot within timeout.")
        child.close(force=True)
        return None
    except pexpect.EOF:
        log("ERROR: QEMU exited unexpectedly during boot.")
        return None


def send_cmd(child, cmd, wait_prompt=True, timeout=30):
    """Send a command and optionally wait for the next prompt."""
    child.sendline(cmd)
    if wait_prompt:
        try:
            child.expect(PROMPT, timeout=timeout)
        except pexpect.TIMEOUT:
            log(f"WARNING: Timeout waiting for prompt after: {cmd}")
        except pexpect.EOF:
            log(f"ERROR: EOF after command: {cmd}")


def run_bench_mixed(child):
    """
    Run bench_mixed and extract the ticks value.
    Returns int ticks or None on failure.
    """
    log("    bench_mixed running — waiting for result...")
    child.sendline("bench_mixed")
    time.sleep(0.5)
    ticks = None

    # ── Step 1: get the ticks value ───────────────────────────────────────────
    try:
        child.expect(r"bench_mixed: all done ticks=(\d+)\r?\n", timeout=TIMEOUT)
        ticks = int(child.match.group(1))
        if ticks < 5:
            log(f"WARNING: captured ticks={ticks} looks like partial match — discarding.")
            return None
    except pexpect.TIMEOUT:
        m = re.search(r"bench_mixed: all done ticks=(\d+)\r?\n", child.before or "")
        if m:
            ticks = int(m.group(1))
            log(f"    (recovered ticks={ticks} from timeout buffer)")
        else:
            log("WARNING: bench_mixed timed out — no result captured.")
            return None
    except pexpect.EOF:
        log("ERROR: EOF while waiting for bench_mixed result.")
        return None

    # ── Step 2: drain the prompt — best effort, never discard a valid result ──
    try:
        child.expect(PROMPT, timeout=10)
    except (pexpect.TIMEOUT, pexpect.EOF):
        pass

    return ticks


def pkill_qemu():
    """Force-kill all qemu-system-x86_64 processes and poll until gone."""
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"],
                   capture_output=True)
    for _ in range(20):
        time.sleep(0.5)
        r = subprocess.run(["pgrep", "-f", "qemu-system-x86_64"],
                           capture_output=True)
        if r.returncode != 0:
            return
    log("WARNING: QEMU process did not exit after 10 seconds.")


def kill_qemu(child):
    """Kill QEMU session — aggressive, polls until confirmed dead."""
    try:
        child.sendcontrol("a")
        time.sleep(0.3)
        child.send("x")
        child.close(force=True)
    except Exception:
        pass
    pkill_qemu()
    log("QEMU killed.")


def run_config(config):
    """
    Boot xv6, configure the scheduler, run bench_mixed RUNS times,
    return list of tick values.
    """
    results = []
    log(f"\n{'='*50}")
    log(f"Config: {config['label']} ({RUNS} runs)")
    log(f"{'='*50}")

    log("Cleaning up any leftover QEMU processes...")
    pkill_qemu()
    time.sleep(3)

    child = boot_xv6()
    if child is None:
        log("Skipping config due to boot failure.")
        return results

    # Apply scheduler config
    for cmd in config["setup"]:
        log(f"Setup: {cmd}")
        send_cmd(child, cmd, timeout=15)
        time.sleep(0.5)

    # Start agent if needed
    for cmd in config["pre_bench"]:
        log(f"Pre-bench: {cmd}")
        send_cmd(child, cmd, timeout=15)
        time.sleep(2)   # give sagent time to register

    # Warm-up: let agent converge before measuring
    if config.get("warmup_runs", 0) > 0:
        log(f"  Warm-up: {config['warmup_runs']} runs (not recorded)...")
        for w in range(config["warmup_runs"]):
            log(f"    warm-up {w+1}/{config['warmup_runs']}")
            run_bench_mixed(child)
            time.sleep(3)
        log("  Warm-up complete — starting measured runs")

    # Run benchmark RUNS times
    for run in range(1, RUNS + 1):
        log(f"  Run {run}/{RUNS}...")
        ticks = run_bench_mixed(child)
        if ticks is not None:
            log(f"  → ticks = {ticks}")
            results.append(ticks)
        else:
            log(f"  → FAILED (None recorded)")
        time.sleep(3) 

    kill_qemu(child)
    return results


def compute_stats(values):
    """Compute median, min, max from a list of values."""
    if not values:
        return {"median": None, "min": None, "max": None, "values": []}
    sorted_v = sorted(values)
    n = len(sorted_v)
    mid = n // 2
    median = sorted_v[mid] if n % 2 != 0 else (sorted_v[mid - 1] + sorted_v[mid]) / 2
    return {
        "median": median,
        "min":    sorted_v[0],
        "max":    sorted_v[-1],
        "values": values,
    }


def main():
    global RUNS
    parser = argparse.ArgumentParser(description="CS461 Benchmark Collector")
    parser.add_argument("--runs", type=int, default=RUNS,
                        help=f"Runs per scheduler config (default: {RUNS})")
    args = parser.parse_args()
    RUNS = args.runs

    log("CS 461 — Benchmark Collection Script")
    log(f"xv6 path: {XV6_PATH}")
    log(f"Runs per config: {RUNS}")
    log(f"Output: {OUTPUT}\n")

    if not os.path.isdir(XV6_PATH):
        log(f"ERROR: xv6 path not found: {XV6_PATH}")
        sys.exit(1)

    log("Pre-flight: killing any existing QEMU instances...")
    pkill_qemu()
    time.sleep(3)

    all_results = {}

    for config in CONFIGS:
        raw = run_config(config)
        all_results[config["name"]] = {
            "label":  config["label"],
            "stats":  compute_stats(raw),
        }
        # Save after each config in case of crash
        with open(OUTPUT, "w") as f:
            json.dump(all_results, f, indent=2)
        log(f"Saved partial results to {OUTPUT}")

    # Final summary
    log("\n" + "="*50)
    log("FINAL SUMMARY")
    log("="*50)
    for name, data in all_results.items():
        s = data["stats"]
        log(f"{data['label']:25s}  median={s['median']}  min={s['min']}  max={s['max']}  values={s['values']}")

    log(f"\nFull results saved to: {OUTPUT}")
    log("Run plot_results.py to generate the chart.")


if __name__ == "__main__":
    main()
