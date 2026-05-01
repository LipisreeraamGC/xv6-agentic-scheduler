#!/usr/bin/env python3
"""
demo_runner.py
--------------
Automatically types demo commands into xv6 running in QEMU.
Each step corresponds to one demo moment in the presentation.

Usage:
    python3 demo_runner.py --step 1    # sagent + schedtop + bench_mixed
    python3 demo_runner.py --step 2    # starvation flood
    python3 demo_runner.py --step 3    # attack_priv security demo
    python3 demo_runner.py --all       # all 3 steps end to end
    python3 demo_runner.py --record    # record with asciinema as backup

Requirements:
    pip3 install pexpect
    brew install asciinema   (for --record only)
"""

import pexpect
import argparse
import time
import sys
import os
import subprocess

# ── Config ────────────────────────────────────────────────────────────────────
XV6_PATH  = os.path.expanduser("~/Desktop/Notes/OS/FTP/xv6-public")
BOOT_WAIT = 45      # seconds to wait for xv6 to boot
PROMPT    = r"\$\s"  # xv6 shell prompt

# Typing delay — slows down commands so they look natural on screen
TYPE_DELAY = 0.06   # seconds between characters (0 = instant)


def log(msg, color="cyan"):
    colors = {"cyan": "\033[96m", "green": "\033[92m",
              "yellow": "\033[93m", "red": "\033[91m", "reset": "\033[0m"}
    print(f"{colors.get(color, '')}{msg}{colors['reset']}", flush=True)


def slow_type(child, text, delay=TYPE_DELAY):
    """Type text character by character for a natural demo look."""
    for ch in text:
        child.send(ch)
        time.sleep(delay)
    child.sendline("")


def wait_prompt(child, timeout=30):
    try:
        child.expect(PROMPT, timeout=timeout)
        return True
    except pexpect.TIMEOUT:
        log("WARNING: Timed out waiting for prompt.", "yellow")
        return False
    except pexpect.EOF:
        log("ERROR: xv6 exited unexpectedly.", "red")
        return False


def boot_xv6():
    """Spawn QEMU and wait for xv6 shell prompt."""
    log("\n🚀 Booting xv6 in QEMU...", "cyan")
    child = pexpect.spawn(
        "make qemu-nox",
        cwd=XV6_PATH,
        encoding="utf-8",
        timeout=BOOT_WAIT,
        echo=False,
        dimensions=(50, 200),
    )
    child.logfile_read = sys.stdout

    try:
        child.expect(PROMPT, timeout=BOOT_WAIT)
        log("\n✅ xv6 booted. Starting demo...\n", "green")
        return child
    except pexpect.TIMEOUT:
        log("ERROR: xv6 did not boot in time.", "red")
        child.close(force=True)
        sys.exit(1)
    except pexpect.EOF:
        log("ERROR: QEMU exited during boot.", "red")
        sys.exit(1)


def step1_sagent_schedtop(child):
    """
    Demo Step 01: Start sagent + schedtop, run bench_mixed.
    Shows: MLFQ demotion live, EMA divergence, AGENT tag.
    """
    log("\n" + "="*60, "cyan")
    log("DEMO STEP 01 — sagent + schedtop + bench_mixed", "cyan")
    log("="*60, "cyan")
    log("Watch: Q column (demotion), FAST/SLOW EMA, SCORE, [AGENT] tag\n", "yellow")

    # Configure MLFQ
    log("► Switching to MLFQ+SJF mode...", "green")
    slow_type(child, "config_mlfq")
    wait_prompt(child, timeout=10)
    time.sleep(0.5)

    # Start agent
    log("► Starting sagent in background...", "green")
    slow_type(child, "sagent &")
    wait_prompt(child, timeout=10)
    time.sleep(2)   # give sagent time to register

    # Start bench_mixed in background
    log("► Starting bench_mixed workload...", "green")
    slow_type(child, "bench_mixed &")
    wait_prompt(child, timeout=10)
    time.sleep(1)

    # Launch schedtop
    log("► Launching schedtop monitor...", "green")
    log("  (Press Ctrl+C after ~20 seconds to continue)\n", "yellow")
    slow_type(child, "schedtop")

    # Let schedtop run for 20 seconds
    try:
        child.expect(PROMPT, timeout=25)
    except pexpect.TIMEOUT:
        # schedtop is still running — send Ctrl+C
        child.sendcontrol("c")
        wait_prompt(child, timeout=5)

    log("\n✅ Step 01 complete.", "green")


def step2_starvation(child):
    """
    Demo Step 02: Flood scheduler with CPU hogs, trigger starvation alert.
    Shows: sagent reactive wakeup, R1+R4 rules firing.
    """
    log("\n" + "="*60, "cyan")
    log("DEMO STEP 02 — Starvation Alert", "cyan")
    log("="*60, "cyan")
    log("Watch: sagent log — qp (queue pressure) spike, R1+R4 rules fire\n", "yellow")

    log("► Launching 4 CPU hogs to flood the scheduler...", "green")
    for i in range(1, 5):
        log(f"  bench_hog {i}/4")
        slow_type(child, "bench_hog &")
        wait_prompt(child, timeout=10)
        time.sleep(0.3)

    log("\n► Watching sagent respond to starvation...", "green")
    log("  (Waiting 15 seconds — watch for R1/R4 in sagent log)\n", "yellow")

    # Wait and show sagent output
    try:
        # Look for starvation signal in sagent output
        child.expect(r"R[14]", timeout=20)
        log("\n🚨 Starvation alert fired! Agent switched to 10-tick wakeup.", "green")
        wait_prompt(child, timeout=10)
    except pexpect.TIMEOUT:
        log("  (No starvation detected in window — hogs may not have demoted yet)", "yellow")
        wait_prompt(child, timeout=5)

    log("\n✅ Step 02 complete.", "green")


def step3_attack_priv(child):
    """
    Demo Step 03: Run attack_priv — rogue process rejected by kernel.
    Shows: privilege escalation blocked, PID gate working.
    """
    log("\n" + "="*60, "cyan")
    log("DEMO STEP 03 — Security: attack_priv", "cyan")
    log("="*60, "cyan")
    log("Watch: setschedparam returns -1 — privilege escalation DENIED\n", "yellow")

    log("► Running attack_priv...", "green")
    slow_type(child, "sec_priv_escalate")

    try:
        child.expect(r"(FAILED|denied|-1)", timeout=15)
        log("\n🔒 Privilege escalation BLOCKED. Kernel returned -1.", "green")
        wait_prompt(child, timeout=10)
    except pexpect.TIMEOUT:
        log("WARNING: Expected rejection output not found.", "yellow")
        wait_prompt(child, timeout=5)

    log("\n✅ Step 03 complete.", "green")
    log("\n🎉 Demo complete! Switch back to slides.", "green")


def run_all(child):
    step1_sagent_schedtop(child)
    time.sleep(1)
    step2_starvation(child)
    time.sleep(1)
    step3_attack_priv(child)


def record_demo():
    """Record the full demo using asciinema."""
    log("\n🎬 Recording demo with asciinema...", "cyan")
    log("This will run all 3 steps and save to demo_backup.cast\n", "yellow")

    try:
        subprocess.run([
            "asciinema", "rec", "demo_backup.cast",
            "--command", f"python3 {__file__} --all --no-boot",
            "--title", "CS461 Agentic Scheduler Demo",
            "--overwrite",
        ], check=True)
        log("\n✅ Recording saved to demo_backup.cast", "green")
        log("Replay with: asciinema play demo_backup.cast", "green")
    except FileNotFoundError:
        log("ERROR: asciinema not found. Install with: brew install asciinema", "red")
    except subprocess.CalledProcessError:
        log("ERROR: Recording failed.", "red")


def main():
    parser = argparse.ArgumentParser(description="CS461 Demo Runner")
    parser.add_argument("--step",    type=int, choices=[1, 2, 3],
                        help="Run a specific demo step (1, 2, or 3)")
    parser.add_argument("--all",     action="store_true",
                        help="Run all 3 demo steps end to end")
    parser.add_argument("--record",  action="store_true",
                        help="Record the demo with asciinema")
    parser.add_argument("--no-boot", action="store_true",
                        help="Skip boot (used internally by --record)")
    args = parser.parse_args()

    if not any([args.step, args.all, args.record]):
        parser.print_help()
        sys.exit(0)

    if args.record:
        record_demo()
        return

    if not os.path.isdir(XV6_PATH):
        log(f"ERROR: xv6 path not found: {XV6_PATH}", "red")
        log("Update XV6_PATH in this script to match your repo location.", "yellow")
        sys.exit(1)

    log("\nCS 461 — Agentic Scheduler Demo Runner", "cyan")
    log(f"xv6 path: {XV6_PATH}", "cyan")

    child = boot_xv6()

    try:
        if args.all:
            run_all(child)
        elif args.step == 1:
            step1_sagent_schedtop(child)
        elif args.step == 2:
            # Step 2 needs sagent already running
            log("Setting up MLFQ + sagent for step 2...", "yellow")
            slow_type(child, "config_mlfq")
            wait_prompt(child, timeout=10)
            slow_type(child, "sagent &")
            wait_prompt(child, timeout=10)
            time.sleep(2)
            step2_starvation(child)
        elif args.step == 3:
            step3_attack_priv(child)

    except KeyboardInterrupt:
        log("\n\nDemo interrupted by user.", "yellow")
    finally:
        log("\nShutting down QEMU...", "cyan")
        try:
            child.sendcontrol("a")
            time.sleep(0.3)
            child.send("x")
            child.close(force=True)
        except Exception:
            pass
        log("Done.", "green")


if __name__ == "__main__":
    main()
