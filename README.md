# Agentic Hybrid MLFQ+SJF Scheduler for xv6

**CS 461 — Operating Systems | Term Project**
**Branch:** `term-project-tier3` | **Platform:** xv6-x64 (x86-64) on QEMU

---

## Overview

This project replaces xv6's default round-robin scheduler with a **three-level MLFQ
(Multi-Level Feedback Queue) scheduler augmented by Shortest-Job-First (SJF) scoring**,
then extends it with a **fully autonomous userspace agent** (`sagent`) that observes
live kernel telemetry and dynamically retunes scheduling parameters at runtime.

The core idea mirrors how production schedulers (Linux CFS, Android EAS) separate
*policy* from *mechanism*: the kernel enforces scheduling rules, while an external
agent adjusts the policy knobs in response to observed workload behaviour — without
requiring kernel recompilation or reboots.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                       USERSPACE                          │
│                                                          │
│   sagent ──────────────────────────────────────────────► │
│    │  observe-decide-actuate loop (every 50 ticks)       │
│    │  6 policy rules → setschedparam()                   │
│    │                                                      │
│   schedtop  ──── polls getschedstats() every 100 ticks   │
│   (live monitor: per-process MLFQ queue, EMA, waits)     │
└──────────────────────────┬───────────────────────────────┘
                           │  3 new syscalls
                   ┌───────▼──────────────────────────┐
                   │           KERNEL                  │
                   │                                   │
                   │  scheduler()   <- proc.c          │
                   │   • MLFQ demotion (trap.c)        │
                   │   • SJF effective_score (aging)   │
                   │   • priority boost (timer)        │
                   │   • rr_mode bypass (RR baseline)  │
                   │                                   │
                   │  schedstat.h <- shared ABI struct │
                   └───────────────────────────────────┘
```

### Kernel Side (Phase 1–2)

| Component | File | Description |
|-----------|------|-------------|
| Scheduler | `proc.c` | 3-level MLFQ + SJF score selection; `rr_mode` flag for RR baseline |
| Telemetry | `trap.c` | Per-tick burst EMA update, demotion, starvation alert, response time |
| ABI | `schedstat.h` | Shared integer-only struct (no strings — prevents prompt injection) |
| Syscalls | `sysproc.c` | `getschedstats`, `setschedparam`, `register_agent` |
| Params | `proc.h` | `struct sched_params` — runtime-tunable scheduling knobs |

### Userspace Side (Phase 3–4)

| Binary | Source | Description |
|--------|--------|-------------|
| `sagent` | `sagent.c` | Autonomous agent: register → observe → decide → actuate loop |
| `schedtop` | `schedtop.c` | Live monitor, prints per-process MLFQ table every 100 ticks |
| `config_rr` | `config_rr.c` | Switches kernel to round-robin mode (evaluation baseline) |
| `config_mlfq` | `config_mlfq.c` | Resets kernel to static MLFQ+SJF defaults |

---

## Syscall Interface

Three new syscalls added at numbers 26, 27, 28 (after `SYS_fgproc = 25`):

```c
/* Fill buf with per-process telemetry; returns count written, -1 on error */
int getschedstats(struct schedstat *buf, int maxproc);

/* Adjust scheduler parameters. Caller MUST be registered agent. */
int setschedparam(struct sched_update *upd);

/* Register this process as the scheduler agent. One-shot; pins caller to Q0. */
int register_agent(void);
```

`struct schedstat` fields (integers only, no strings):

| Field | Meaning |
|-------|---------|
| `pid`, `state` | Process identity and state (RUNNABLE=3, RUNNING=4) |
| `queue_level` | Current MLFQ queue: 0 (highest) → 2 (lowest) |
| `burst_ema_fast/slow` | Fast/slow EMA of CPU burst length (×100, integer) |
| `wait_ticks` | Ticks spent RUNNABLE but not scheduled |
| `yield_count` / `preempt_count` | Voluntary yields vs forced preemptions |
| `burst_variance` | \|fast−slow\| — phase-change detector |
| `effective_score` | SJF priority score with aging (lower = higher priority) |
| `sched_alert` | Non-zero when starvation detected (valid on entry[0] only) |
| `first_run_tick` / `create_tick` | Response time = `first_run_tick − create_tick` |

---

## Agent Policy Rules

`sagent` registers with the kernel, then runs an **observe-decide-actuate** loop
every 50 ticks (10 ticks when `sched_alert` is set). Each epoch it computes four
signals and applies six rule-based policies:

**Signals computed each epoch:**

| Signal | Meaning |
|--------|---------|
| `avg_wait` | Mean `wait_ticks` across RUNNABLE processes |
| `queue_pressure` | % of RUNNABLE processes stuck in Q2 (0–100) |
| `workload_type` | Ratio of voluntary yields to total switches (0=CPU, 100=I/O) |
| `phase_change` | Non-zero when `burst_variance` spikes (workload shift detected) |
| `abuse_detected` | Non-zero when yield rate drops sharply (SJF gaming attempt) |

**Policy rules (with 1-epoch deadband to prevent oscillation):**

| Rule | Trigger | Action |
|------|---------|--------|
| R1 | `queue_pressure > 40` | Decrease boost interval; decrease Q0 quantum |
| R2 | `avg_wait > 10` | Decrease boost interval (starvation mitigation) |
| R3 | `workload_type > 70` | Increase EMA alpha_fast (I/O-bound detected) |
| R4 | `avg_wait < 10 && queue_pressure < 20` | Increase boost interval (system underloaded) |
| R5 | `phase_change detected` | Increase alpha_fast; if no phase change, decay it |
| R6 | `abuse_detected` | Decrease aging factor (penalise SJF gaming) |

Each epoch logs signals, fired rules, and new parameter values to stdout.

---

## Security Model

`setschedparam` is restricted to the registered agent via kernel-enforced `agent_pid`
check — any non-agent call returns `-1` immediately. Six userspace demos exercise
the security model:

| Binary | Attack Simulated | Expected Result |
|--------|-----------------|-----------------|
| `sec_priv_escalate` | Calls `setschedparam` without `register_agent` | Kernel rejects (−1) |
| `sec_param_corrupt` | Passes out-of-range parameters | Kernel clamps/rejects |
| `sec_fork_bomb` | Rapid `fork()` to exhaust scheduler slots | Scheduler stable |
| `sec_sjf_abuse` | Periodic `sleep()` to fake short burst / game SJF | R6 fires |
| `sec_behavioral_mimic` | Mimics I/O pattern to stay in Q0 | R3/R6 combination |
| `sec_prompt_inject` | Integer-only ABI prevents string-based injection | Field ignored by design |

`attack_priv.c` is a standalone single-syscall test of the privilege check.

---

## Benchmarks

Three workloads, three scheduler configurations:

| Binary | Workload | Description |
|--------|----------|-------------|
| `bench_cpu` | CPU-bound | 100M-iteration arithmetic loop; drives process to Q2 |
| `bench_io` | I/O-bound | 200× `sleep(1)`; stays in Q0 |
| `bench_mixed` | Mixed (tier3) | 6 CPU-bound + 4 I/O-bound children; parent waits for all 10 |

**Tier3 suboptimal defaults** (agent starts from intentionally degraded params and tunes toward optimal):

| Parameter | Suboptimal Default | Optimal Target |
|-----------|-------------------|----------------|
| Q0 quantum | 4 | 1 |
| Q1 quantum | 6 | 2 |
| Q2 quantum | 8 | 4 |
| Boost interval | 500 | 100 |
| Aging factor | 50 | 10 |

---

## Evaluation Results

**Setup:** QEMU x86-64, `-m 512`, `CPUS=2` (SMP), `-nographic`. `bench_mixed` runs
in isolated QEMU sessions to eliminate cross-run `kalloc()` pressure. n=15 per config
(tier3), n=5 per (config, workload) pair (phase4e).

### Tier3 — `bench_mixed` (6 CPU + 4 IO), n=15

| Config | Median (ticks) | Min | Max | vs RR |
|--------|---------------|-----|-----|-------|
| Round-Robin | 325 | 324 | 326 | baseline |
| Static MLFQ+SJF | 306 | 300 | 321 | **−5.8%** |
| MLFQ+SJF + Agent | 305 | 300 | 332 | **−6.2%** |

> Mann-Whitney U p < 0.0001 (RR vs MLFQ). Agent vs MLFQ delta is ~1 tick (essentially
> noise at this run count), but tuning IS visible: `boost_interval` converges from 500
> to ~10, `q0_quantum` from 4 to 1 within the first few epochs.

### Phase4e — All Workloads (Turnaround, n=5)

| Workload | RR | MLFQ | Agent | MLFQ Δ% | Agent Δ% |
|----------|----|------|-------|---------|---------|
| bench_cpu | 47 | 49 | 48 | −4.3% | −2.1% |
| bench_io | 200 | 200 | 200 | 0.0% | 0.0% |
| bench_mixed | 281 | 262 | 251 | **+6.8%** | **+10.7%** |

> bench_mixed improvements for both MLFQ and agent are statistically significant
> at α=0.05 (Mann-Whitney p=0.012). Single-workload cases show no significant
> difference — cross-process priority effects only surface under mixed workloads.

**Known limitations:**

- Agent doesn't meaningfully beat static MLFQ in tier3: (1) `burst_ema` overshoots
  optimal at bst=10; (2) only Q0 quantum is tuned by R1, Q1/Q2 never decrease;
  (3) syscall overhead per epoch costs ~1 tick. Tuning IS visible but doesn't fully
  translate to faster turnaround — documented honestly in the report.
- No userspace `yield` syscall; `yield_count` only increments via kernel preemption.
  `sec_sjf_abuse.c` uses `sleep(1)` as the voluntary-yield substitute.

---

## How to Build and Run

```bash
# Build xv6 with the scheduler
make

# Boot in QEMU (2 CPUs)
make qemu CPUS=2
```

Inside the xv6 shell:

```sh
# Round-robin baseline
$ config_rr

# Static MLFQ+SJF
$ config_mlfq

# MLFQ+SJF with autonomous agent
$ config_mlfq
$ sagent &

# Live monitor
$ schedtop

# Benchmarks
$ bench_cpu
$ bench_io
$ bench_mixed

# Security demos
$ sec_priv_escalate
$ sec_fork_bomb
$ sec_sjf_abuse
$ attack_priv
```

**Automated evaluation** (from host, requires `pexpect`):

```bash
python3 run_eval.py      # runs all 3 configs, collects turnaround ticks
python3 compute_stats.py # Mann-Whitney U + bootstrap CI on results
```

---

## Key Design Decisions

| Decision | Reason |
|----------|--------|
| Integer-only `schedstat.h` ABI | Prevents prompt injection if telemetry is fed to an LLM reasoning step |
| `agent_pid` kernel guard on `setschedparam` | Privilege separation: only registered agent can tune params |
| `rr_mode` field in scheduler | True RR bypass in `scheduler()` — defensible vs. parameter-tweaking approximation |
| `first_run_tick` recorded in `scheduler()` dispatch | Cheapest place to capture response time without per-state hooks |
| `create_tick` in `allocproc()` | Response time = `first_run_tick − create_tick`; using `uptime()` inside `main()` was wrong — process is first scheduled during the kernel `execve` path, before `main()` runs |
| `exit()` clears `agent_pid` | One-shot guard would otherwise be permanently latched after first registrant exits |
| 1-epoch deadband on rules R1–R4, R6 | Prevents oscillation from single-epoch spikes; R5 exempt because it has a symmetric decay branch |
| Suboptimal tier3 defaults | Agent needs room to tune downward — starting at optimal means agent adds only syscall overhead |
| Lock ordering: `ptable.lock` first, then `schedp.lock` | Consistent acquire order enforced at all sites to prevent deadlock |

---

## File Index

| File | Phase | Purpose |
|------|-------|---------|
| `proc.h` | 1 | `struct proc` (11 MLFQ fields), `struct sched_params` |
| `proc.c` | 1 | `scheduler()`, MLFQ selection, RR bypass, `kern_pin_agent()` |
| `trap.c` | 1 | Timer telemetry, EMA update, demotion, starvation alert |
| `schedstat.h` | 2 | Shared ABI struct (kernel + userspace) |
| `sysproc.c` | 2 | `sys_getschedstats`, `sys_setschedparam`, `sys_register_agent` |
| `sagent.c` | 3 | Autonomous agent: 6-rule policy, observe-decide-actuate loop |
| `schedtop.c` | 4 | Live monitor — polls every 100 ticks, prints MLFQ table |
| `bench_cpu.c` | 4 | CPU-bound benchmark (100M iters) |
| `bench_io.c` | 4 | I/O-bound benchmark (200× sleep) |
| `bench_mixed.c` | tier3 | Mixed workload: 6 CPU + 4 IO children |
| `config_rr.c` | 4e | Sets `rr_mode=1` (RR baseline) |
| `config_mlfq.c` | 4e | Resets `rr_mode=0` + default MLFQ params |
| `sec_*.c` (×6) | 4e | Security demos |
| `attack_priv.c` | 4e | Single-shot privilege escalation test |
| `eval_results.md` | 4e | Full raw data + statistical analysis (n=5) |
| `run_eval.py` | 4e | Automated evaluation harness (pexpect) |
| `compute_stats.py` | 4e | Mann-Whitney U + bootstrap CI script |

---

## Author

Lipisreeraam GC — UIC MS ECE, Spring 2026
CS 461 Operating Systems — Term Project
