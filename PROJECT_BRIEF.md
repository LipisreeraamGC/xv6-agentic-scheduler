# Project Brief: Agentic Hybrid MLFQ+SJF Scheduler for xv6

> Prepared 2026-04-26 | Branch: `term-project-phase4` | Course: ECE 461 Operating Systems

---

## 1. One-Sentence Summary

A teaching operating system kernel (xv6) extended with an intelligent, self-tuning process scheduler: a userspace "agent" program autonomously monitors kernel telemetry and adjusts scheduling parameters in real time to improve throughput and fairness across mixed CPU/IO workloads.

---

## 2. Problem It Solves

**Status quo before:** xv6 uses a trivially simple round-robin scheduler — every runnable process gets one timer tick, in order. There is no distinction between CPU-bound and I/O-bound processes, no mechanism to prevent starvation, and no way to tune behavior without recompiling the kernel.

**Real-world analog:** Production OS schedulers (Linux CFS, Windows HPET, Android EAS) must continuously adapt to mixed workloads — interactive apps, background daemons, batch jobs — without requiring a kernel rebuild. The gap between xv6's static round-robin and a production scheduler is enormous.

**This project bridges that gap** by implementing:
1. A hybrid MLFQ+SJF kernel scheduler (priority queues + shortest-job-first selection)
2. A syscall-based telemetry and control interface
3. An autonomous userspace agent that observes kernel state and dynamically re-tunes the scheduler each epoch — all without ever rebooting or recompiling the kernel

---

## 3. Technical Approach (How It Works)

### Phase 1 — Hybrid MLFQ+SJF Kernel Scheduler (`proc.c`, `trap.c`)

The default round-robin `scheduler()` function in `proc.c` was replaced with a 4-step algorithm that runs on every scheduler invocation:

1. **Priority boost** (anti-starvation): Every `boost_interval` ticks, all processes are promoted back to Q0 (highest priority) and their quantum/wait counters reset.
2. **Aging**: Every RUNNABLE process has its `wait_ticks` incremented each scheduling pass. When any process exceeds 100 ticks of waiting, the kernel sets `schedp.sched_alert = 1` to wake the agent immediately.
3. **MLFQ+SJF selection**: Queues are checked in priority order (Q0 → Q1 → Q2). Within the first non-empty queue, the process with the lowest `effective_score` wins. The score is `burst_ema_fast/100 - wait_ticks/aging_factor`, so short-burst processes win, but long-waiting processes eventually overcome them.
4. **Dispatch**: The winner is context-switched in; its burst start tick is recorded for EMA update on the next timer interrupt.

**Timer interrupt path (`trap.c`)**: On each tick, if a process is running, `ticks_remaining` is decremented. When it hits zero, the quantum expired: both fast and slow EMAs are updated using fixed-point integer arithmetic (scaled ×100), burst variance is computed, `preempt_count` is incremented, and the process is demoted one queue level (unless it is the pinned agent).

### Phase 2 — Syscall Interface (`sysproc.c`, `schedstat.h`)

Three new syscalls (numbers 26, 27, 28) form the kernel-agent API:

- **`getschedstats(buf, maxproc)`**: Copies a typed snapshot of all non-UNUSED processes into userspace. Validates the entire buffer range before copying. Piggybacks `sched_alert` on `buf[0]` to avoid a second syscall.
- **`setschedparam(struct sched_update *)`**: Accepts a pointer to a 7-field struct (to stay within the x86-64 6-register syscall ABI limit). Validates all 7 parameters (bounds + monotonicity invariants). Only the registered agent's PID may call this; all others get `-1`.
- **`register_agent()`**: One-shot TOCTOU-safe registration using a double-check under `schedp.lock`. Pins the caller permanently to Q0 via `kern_pin_agent()`.

### Phase 3 — Autonomous Agent (`sagent.c`)

The agent runs an observe-decide-actuate loop:

1. **Observe**: Call `getschedstats()`, read per-process telemetry
2. **Compute signals**:
   - `avg_wait`: mean wait ticks across RUNNABLE processes
   - `queue_pressure`: fraction of RUNNABLE processes stuck in Q2 (0–100)
   - `workload_type`: yield/(yield+preempt) ratio (0=CPU-bound, 100=IO-bound)
   - `phase_change`: average |fast_EMA − slow_EMA| divergence > 50
   - `sjf_abuse`: burst_variance < 5 AND yield_count > 20 (strategic yielding)
3. **Apply 6 policy rules** (each fires independently, result is a bitmask):
   - R1: Q2 crowding → decrease `boost_interval` (boost more frequently)
   - R2: IO-heavy → increase Q0 quantum (reward yielders)
   - R3: CPU-heavy → decrease Q0 quantum (tighten fairness)
   - R4: High average wait → emergency boost interval reduction
   - R5: Phase change → increase EMA `alpha_fast`; else decay it
   - R6: SJF abuse detected → decrease `aging_factor` (aging overrides burst score faster)
4. **Actuate**: Send updated `struct sched_update` to kernel via `setschedparam()`
5. **Log**: Print one line per epoch listing which rules fired and the resulting parameter values
6. **Sleep**: Normal = 50 ticks; starvation alert = 10 ticks (responsive mode)

### Phase 4 — Monitoring & Benchmarks

- **`schedtop`**: Live htop-like monitor. Polls `getschedstats()` every 100 ticks; prints a table of PID, queue level, EMA values, wait, yield/preempt counts, and effective score. Displays starvation alert banner.
- **`bench_cpu`**: 100M-iteration arithmetic loop → drives process to Q2
- **`bench_io`**: 200 × `sleep(1)` → stays in Q0/Q1 (voluntary yields)
- **`bench_mixed`**: Forks 4 CPU children + 4 IO children, waits for all 8

---

## 4. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                        USERSPACE                            │
│                                                             │
│  sagent (PID N, Q0-pinned)     schedtop       bench_*       │
│    observe-decide-actuate         monitor      workloads     │
│         loop                   (read-only)                  │
│           │                        │                        │
│    setschedparam()           getschedstats()                │
│    register_agent()                │                        │
└───────────────────┬────────────────┼────────────────────────┘
                    │ syscall        │ syscall
┌───────────────────▼────────────────▼────────────────────────┐
│                        KERNEL                               │
│                                                             │
│  sysproc.c                                                  │
│  ├─ sys_register_agent()  → kern_pin_agent()                │
│  ├─ sys_setschedparam()   → validates + updates schedp      │
│  └─ sys_getschedstats()   → kern_getschedstats()            │
│                                                             │
│  proc.c: scheduler()          trap.c: trap()                │
│  ├─ priority boost            ├─ tick: ticks_remaining--    │
│  ├─ aging + alert             ├─ EMA update (fast + slow)   │
│  ├─ MLFQ+SJF selection        ├─ variance computation       │
│  └─ dispatch + burst start    ├─ preempt_count++            │
│                               └─ MLFQ demotion              │
│                                                             │
│  schedp (struct sched_params)  ← tunable live parameters   │
│  ptable (struct proc[64])      ← per-process MLFQ fields   │
└─────────────────────────────────────────────────────────────┘

Locks:  ptable.lock → schedp.lock  (always acquire in this order)
```

**Key design invariant:** The scheduler reads `schedp` fields without holding `schedp.lock` (individual int reads are atomic on x86-64). The agent updates `schedp` under `schedp.lock`. This avoids deadlock between the scheduler and the agent system call path.

---

## 5. Key Results / Metrics

| Workload | Baseline (round-robin) | Agent-tuned MLFQ+SJF | Notes |
|---|---|---|---|
| `bench_cpu` (standalone) | ~65 ticks | **47 ticks** | ~28% faster; SJF concentrates CPU time |
| `bench_io` (standalone) | 200 ticks | **200 ticks** | Exact (sleep-dominated; scheduler irrelevant) |
| `bench_mixed` (4+4) | ~310 ticks | **250 ticks** | ~19% faster; IO children finish sooner in Q0 |

- **12 per-process telemetry fields** exported via a single syscall
- **7 tunable scheduler parameters** modifiable live without kernel restart
- **6 policy rules** cover: starvation, workload classification, phase detection, abuse mitigation
- **Double EMA** (α=0.5 fast, α=0.1 slow) detects workload phase changes within 2–3 epochs
- **Starvation response time**: ≤10 ticks (agent wakes on `sched_alert` flag)

---

## 6. Tech Stack

| Layer | Technology |
|---|---|
| OS / Kernel | xv6-x64 (x86-64 port of MIT xv6) |
| Language | K&R C (C89 dialect) — no C99 or later |
| Architecture | x86-64, 4KB pages, 2-CPU SMP |
| Build | GNU Make, x86_64-elf-gcc (macOS cross-compiler) |
| Runtime | QEMU (`qemu-system-x86_64`) |
| Synchronization | Spinlocks (`struct spinlock`) — no sleeping locks in scheduler |
| Tooling | clangd LSP, cscope, gdb via `.gdbinit.tmpl` |
| Version Control | Git, hosted on GitHub (`sysec-uic/xv6-public` fork) |

---

## 7. My Specific Contribution

**Everything in this project was built from scratch on top of the base xv6-x64 starter**, which provides only the round-robin scheduler and no scheduling telemetry.

Specifically built:

| Component | What I built |
|---|---|
| `proc.c` `scheduler()` | Replaced round-robin with 4-step MLFQ+SJF + starvation alert |
| `proc.c` `allocproc()` | Added 11 MLFQ fields to per-process initialization |
| `trap.c` timer handler | Added EMA update, variance, preempt_count, demotion logic |
| `proc.h` | Added `struct sched_params`, 11 MLFQ fields to `struct proc` |
| `schedstat.h` | Designed the kernel-userspace ABI struct (security-conscious: integers only) |
| `sysproc.c` | Implemented all 3 new syscalls with bounds checking and TOCTOU safety |
| `sagent.c` | Autonomous agent: 5 signal computations, 6 policy rules, per-epoch logging |
| `schedtop.c` | Live scheduler monitor (htop-style terminal output) |
| `bench_cpu/io/mixed.c` | 3 benchmarks across the CPU/IO spectrum |

**Key design decisions made:**
- Using a **pointer-based struct** for `setschedparam` instead of 7 individual arguments (x86-64 syscall ABI only passes 6 registers)
- Using **fixed-point integer arithmetic** (scaled ×100) for EMA — no floating point in kernel
- Using **integer-only `schedstat` fields** explicitly as an anti-prompt-injection defense
- **TOCTOU-safe `register_agent`** using double-check under `schedp.lock`
- **Piggybacking `sched_alert`** on `buf[0]` of `getschedstats` to avoid a 4th syscall
- Setting `sched_alert` in `scheduler()` rather than `trap.c` (at trap time, `wait_ticks` has already been reset to 0 by the scheduler, making it impossible to detect starvation there)

---

## 8. Challenges & How I Solved Them

### Challenge 1: x86-64 Syscall ABI — 6-Register Limit
The original design passed 7 scheduling parameters as individual `argint()` arguments to `setschedparam`. On x86-64, syscalls only pass 6 argument registers; the 7th caused a kernel panic (`failed fetch` on `argint(6)`).

**Solution:** Bundled all 7 parameters into `struct sched_update` and passed a userspace pointer. This reduced the syscall to 1 argument and added a natural validation point (validate the struct before reading it).

### Challenge 2: Scheduler/Agent Lock Ordering and Deadlock Risk
The scheduler runs under `ptable.lock` and reads `schedp` fields. The agent's `setschedparam` syscall must update `schedp`. If both held each other's locks, deadlock would occur.

**Solution:** The scheduler **never acquires `schedp.lock`** — it reads `schedp` fields as raw `int` reads (atomic on x86). Only the syscall path holds `schedp.lock`, and only briefly. Lock ordering is documented: acquire `ptable.lock` first, then `schedp.lock` if both are ever needed.

### Challenge 3: Starvation Alert Timing Bug
Early versions set `sched_alert` in `trap.c` when a quantum expired. But `wait_ticks` is reset to 0 at dispatch time in `scheduler()` — meaning by the time the timer interrupt fires, `wait_ticks` is already 0 for the running process. Starvation was never detected.

**Solution:** Moved the `sched_alert` check to `scheduler()` during the aging pass — at the moment when `wait_ticks` is being incremented, before dispatch resets it.

---

## 9. What I'd Do Differently

1. **RL bandit for parameter tuning.** The 6 policy rules are hand-coded thresholds. An epsilon-greedy bandit with integer arithmetic (the "stretch goal") would adapt automatically to workloads the rules don't cover. The fixed-point integer constraint means Q-values would be scaled ×1000, but it's achievable.

2. **Per-CPU scheduling.** The current scheduler uses a single global `ptable.lock`, which serializes scheduling decisions across all CPUs. A per-run-queue design (like Linux's O(1) scheduler) would eliminate the lock bottleneck on multi-CPU QEMU.

3. **Formal safety bounds on agent parameters.** The current approach clamps each parameter independently. A Lyapunov-style argument could prove that the clamped parameter space always converges (no oscillation), which would be stronger than empirical testing.

4. **More rigorous evaluation methodology.** The 3×3×5 performance matrix (3 configs, 3 workloads, 5 runs) is still pending. The current baseline numbers are single-run; statistical significance requires the full 45 runs.

---

## 10. Business / Real-World Impact

**Industry mapping:** This project directly maps to work done by OS kernel engineers at companies like Google, Meta, and Apple, where scheduling is a live research area.

- **Google's Borg/Kubernetes** schedulers use similar observe-actuate loops — the agent pattern here mirrors a "scheduler extender" in k8s
- **Android EAS (Energy Aware Scheduling)** uses workload classification (CPU-bound vs IO-bound) to select between efficiency and performance cores — the same signal this agent computes from yield/preempt ratios
- **Linux CFS** uses EWMA (Exponentially Weighted Moving Average) for load tracking; this project implements the identical formula in fixed-point integer arithmetic in a teaching OS
- **Cloudflare and Netflix** have published work on userspace schedulers that observe kernel events via eBPF and adjust behavior dynamically — this project is the educational analog of that pattern

**The security design** (integer-only telemetry ABI, agent-only writes, TOCTOU-safe registration) demonstrates awareness of the threat model for AI-enhanced kernel components — a topic receiving increasing attention as ML is deployed closer to kernel boundaries.

---

## 11. Likely Interview / Business Questions & Answers

**Q1: Why not just use Linux's CFS instead of building your own scheduler?**
> CFS is ~10,000 lines of highly optimized C tuned for decades across diverse hardware. Building on xv6 lets you understand the primitives — priority queues, EMA-based burst estimation, aging, priority boost — from first principles without the abstraction layers. The concepts transfer directly; the implementation is educational.

**Q2: How does the agent avoid starving itself while tuning the scheduler?**
> The agent is registered via `register_agent()`, which calls `kern_pin_agent()` to permanently fix the agent's `queue_level = 0` and prevent MLFQ demotion in the timer interrupt. The scheduler checks `schedp.agent_pid` before demoting any process. The agent also sleeps most of the time (50 ticks between polls), so it consumes nearly zero CPU.

**Q3: What happens if the agent crashes or is killed?**
> `schedp.agent_pid` is never cleared. The kernel continues running with the last parameters the agent set — degraded but functional. A production-grade version would add a watchdog (e.g., `register_agent` could store a "last seen" tick that the scheduler checks; if the agent hasn't updated in N epochs, reset to defaults). This is a known limitation.

**Q4: How do you prevent a malicious process from calling `setschedparam` and corrupting the scheduler?**
> `sys_setschedparam` returns `-1` immediately if `proc->pid != schedp.agent_pid`. Since `schedp.agent_pid` is set once under a spinlock and never cleared, only the exact registered process (by PID) can update parameters. The bounds validation on all 7 fields ensures even a malicious agent cannot push parameters outside safe ranges.

**Q5: Why integer-only arithmetic in the kernel? Couldn't you use floats?**
> x86-64 floating-point uses the FPU/SSE unit, which has its own register state (`xmm0`–`xmm15`, `mxcsr`). In the kernel, saving and restoring this state on every context switch is expensive — xv6 avoids it entirely. Fixed-point arithmetic (scale by 100 or 1000) achieves the same precision for scheduling purposes with zero FPU overhead.

**Q6: How does the dual-EMA phase change detection work?**
> Two exponential moving averages track the same signal (CPU burst length) but with different alpha (responsiveness) values: fast (α=0.5) reacts to each burst quickly, slow (α=0.1) is a long-term baseline. When a workload transitions (e.g., from CPU-bound to IO-bound), the fast EMA drops quickly while the slow EMA still reflects the old behavior. The divergence |fast − slow| > 50 (scaled) is the phase-change trigger. This is analogous to MACD in financial technical analysis.

**Q7: What is the computational cost of running the agent?**
> The agent sleeps for 50 ticks between epochs (~0.5 seconds at 100 Hz). Each epoch: one `getschedstats` syscall (O(NPROC) = O(64) iterations, all under `ptable.lock`), 5 signal computations (each O(64)), 6 conditional checks, and one `setschedparam` syscall. Total CPU time per epoch is on the order of a few hundred instructions — negligible compared to the workloads being scheduled.

**Q8: How would you scale this to a real multi-core machine?**
> Three changes: (1) Per-CPU run queues to eliminate the single global `ptable.lock` bottleneck, (2) Per-CPU agent telemetry aggregation before the agent reads (or one agent per CPU with a coordinator), (3) Atomic parameter updates using `cmpxchg` instead of a global spinlock. The agent's logic itself is already stateless enough to run per-CPU.

---

## 12. Buzzwords & Keywords

**OS / Scheduling:**
MLFQ, Multi-Level Feedback Queue, SJF, Shortest Job First, priority boost, process starvation, aging, preemption, quantum, context switch, round-robin, CFS (Completely Fair Scheduler), EAS (Energy Aware Scheduling), Borg scheduler

**Systems Programming:**
xv6, x86-64, kernel development, spinlock, critical section, TOCTOU, lock ordering, fixed-point arithmetic, timer interrupt, trap handler, syscall ABI, EMA (Exponentially Weighted Moving Average), telemetry

**Architecture Patterns:**
observe-decide-actuate loop, agentic system, autonomous agent, kernel-userspace interface, privilege separation, kernel bypass, eBPF (conceptual analog), scheduler extender

**Security:**
privilege escalation prevention, parameter bounds validation, integer-only ABI (prompt injection defense), PID-gated access control, TOCTOU-safe registration

**Performance:**
turnaround time, response time, throughput, CPU-bound vs I/O-bound workload classification, yield/preempt ratio, burst estimation, phase change detection, starvation detection

**Industry Mapping:**
Linux kernel, Android EAS, Google Borg, Kubernetes scheduler, cloud-native scheduling, ML-enhanced scheduling, adaptive systems, control theory (Lyapunov stability)

**Languages & Tools:**
C (K&R / C89), QEMU, GNU Make, GDB, clangd, cscope, x86_64-elf-gcc, Git
