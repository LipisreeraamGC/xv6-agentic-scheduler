/*
 * schedstat.h -- per-process telemetry snapshot for the MLFQ+SJF scheduler.
 *
 * Included by both kernel (via proc.h) and userspace (via user.h / sagent.c).
 *
 * SECURITY: integer fields ONLY.  No name[], no argv, no pointers.
 * This is intentional -- prevents prompt injection if the agent ever
 * feeds raw telemetry into an LLM reasoning step.
 *
 * IMPROVEMENT over spec: added `state` and `sched_alert` fields so the
 * agent can skip non-runnable processes and react to starvation alerts
 * without needing a separate syscall.
 */

#ifndef SCHEDSTAT_H
#define SCHEDSTAT_H

struct schedstat {
  int pid;
  int state;           /* process state: RUNNABLE=3, RUNNING=4 (enum procstate) */
  int queue_level;     /* MLFQ queue: 0=highest priority, 2=lowest */
  int burst_ema_fast;  /* fast EMA of CPU burst length, scaled by 100 */
  int burst_ema_slow;  /* slow EMA of CPU burst length, scaled by 100 */
  int wait_ticks;      /* scheduling decisions spent RUNNABLE but not picked */
  int yield_count;     /* voluntary context switches -- proxy for I/O-bound behavior */
  int preempt_count;   /* forced preemptions when quantum expired */
  int burst_variance;  /* |burst_ema_fast - burst_ema_slow|: phase-change detector */
  int page_fault_count;
  int effective_score; /* SJF score with aging: lower = higher priority */
  int sched_alert;     /* non-zero when starvation detected; valid on entry[0] only */
  int first_run_tick;  /* uptime tick when process was first scheduled; 0 = never */
  int create_tick;     /* uptime tick when allocproc() ran (job arrival time) */
};

/* Passed by pointer to setschedparam() -- avoids the x86-64 6-register limit */
struct sched_update {
  int q0_quantum;
  int q1_quantum;
  int q2_quantum;
  int boost_interval;
  int ema_alpha_fast;
  int ema_alpha_slow;
  int aging_factor;
  int rr_mode;          /* 1 = round-robin bypass; 0 = MLFQ+SJF */
};

#endif
