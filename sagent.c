/*
 * sagent.c -- Autonomous scheduling agent for xv6 MLFQ+SJF scheduler.
 * Implements observe-decide-actuate loop.
 * Registers with kernel via register_agent(), then polls getschedstats()
 * every EPOCH_TICKS ticks and calls setschedparam() to tune the scheduler.
 */

#include "types.h"
#include "user.h"
#include "schedstat.h"

#define NPROC        64
#define EPOCH_TICKS  50    /* normal poll interval in ticks */
#define ALERT_SLEEP  10    /* shorter poll when sched_alert is set */

/* Current parameter state -- adjusted by decide_and_update() each epoch */
static int cur_q0    = 4;
static int cur_q1    = 6;
static int cur_q2    = 8;
static int cur_boost = 500;
static int cur_af    = 50;   /* ema_alpha_fast * 100 */
static int cur_as    = 10;   /* ema_alpha_slow * 100 */
static int cur_aging = 50;
static int epoch     = 0;

/* ------------------------------------------------------------------ */
/* Signal computation helpers                                          */
/* ------------------------------------------------------------------ */

/*
 * compute_avg_wait -- mean wait_ticks across RUNNABLE processes.
 * High value means processes are sitting idle too long.
 */
static int
compute_avg_wait(struct schedstat *stats, int n)
{
    int sum, i, count;
    sum = 0;
    count = 0;
    for(i = 0; i < n; i++){
        if(stats[i].state == 3){ /* RUNNABLE == 3 in enum procstate */
            sum += stats[i].wait_ticks;
            count++;
        }
    }
    if(count == 0)
        return 0;
    return sum / count;
}

/*
 * compute_queue_pressure -- fraction of RUNNABLE procs stuck in Q2 (0-100).
 * High value means many processes have been demoted; quanta may be too tight.
 */
static int
compute_queue_pressure(struct schedstat *stats, int n)
{
    int count_q2, count_runnable, i;
    count_q2 = 0;
    count_runnable = 0;
    for(i = 0; i < n; i++){
        if(stats[i].state == 3){ /* RUNNABLE */
            count_runnable++;
            if(stats[i].queue_level == 2)
                count_q2++;
        }
    }
    if(count_runnable == 0)
        return 0;
    return (count_q2 * 100) / count_runnable;
}

/*
 * compute_workload_type -- ratio of voluntary yields to total context switches.
 * Returns 0-100: high = I/O-bound (lots of yields), low = CPU-bound.
 */
static int
compute_workload_type(struct schedstat *stats, int n)
{
    int total_yield, total_preempt, i;
    total_yield = 0;
    total_preempt = 0;
    for(i = 0; i < n; i++){
        total_yield   += stats[i].yield_count;
        total_preempt += stats[i].preempt_count;
    }
    if(total_yield + total_preempt == 0)
        return 50; /* no data -- assume mixed */
    return (total_yield * 100) / (total_yield + total_preempt);
}

/*
 * detect_phase_change -- large average divergence between fast and slow EMA
 * signals a workload phase transition (e.g. CPU-bound suddenly becomes I/O-bound).
 */
static int
detect_phase_change(struct schedstat *stats, int n)
{
    int divergence, diff, i;
    divergence = 0;
    for(i = 0; i < n; i++){
        diff = stats[i].burst_ema_fast - stats[i].burst_ema_slow;
        if(diff < 0)
            diff = -diff;
        divergence += diff;
    }
    if(n > 0)
        divergence = divergence / n;
    return divergence > 50; /* 1 = phase change detected */
}

/*
 * detect_sjf_abuse -- low burst_variance + high yield_count is a sign that
 * a process is gaming SJF by yielding strategically to keep its EMA low.
 */
static int
detect_sjf_abuse(struct schedstat *stats, int n)
{
    int i;
    for(i = 0; i < n; i++){
        if(stats[i].burst_variance < 5 && stats[i].yield_count > 20)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Policy rules                                                        */
/* ------------------------------------------------------------------ */

static int
clamp(int val, int lo, int hi)
{
    if(val < lo) return lo;
    if(val > hi) return hi;
    return val;
}

/*
 * decide_and_update -- apply rule-based policy to compute new parameters.
 * Returns a bitmask of which rules fired this epoch (bits 0-5 = rules 1-6).
 * All updates are clamped to legal ranges before setschedparam() is called.
 */
static int
decide_and_update(int avg_wait, int queue_pressure, int workload_type,
                  int phase_change, int abuse_detected)
{
    /*
     * cnt[i] counts consecutive epochs the condition for rule i+1 was true.
     * A rule only fires once the signal has been sustained for 2 epochs,
     * preventing oscillation from single-epoch spikes.
     * Rule 5 (phase_change) is exempt: it has a symmetric decay branch that
     * must run every epoch, so a deadband would block the settle path.
     */
    static int cnt[6];   /* zero-initialised (static storage) */
    int fired;
    fired = 0;

    /* Rule 1: Q2 overloaded -- boost more frequently */
    if(queue_pressure > 40) cnt[0]++;
    else                     cnt[0] = 0;
    if(cnt[0] >= 1){
        cur_boost = clamp(cur_boost - 20, 10, 500);
        cur_q0 = clamp(cur_q0 - 1, 1, 10);
        fired |= 0x01;
    }

    /* Rule 2: I/O-dominant -- widen Q0 quantum to reward yielders */
    if(workload_type > 70) cnt[1]++;
    else                   cnt[1] = 0;
    if(cnt[1] >= 2){
        cur_q0 = clamp(cur_q0 + 1, 1, 10);
        fired |= 0x02;
    }

    /* Rule 3: CPU-dominant -- tighten Q0 to reduce hogging */
    if(workload_type < 30) cnt[2]++;
    else                   cnt[2] = 0;
    if(cnt[2] >= 2){
        cur_q0 = clamp(cur_q0 - 1, 1, 10);
        fired |= 0x04;
    }

    /* Rule 4: starvation -- rescue via tighter boost interval */
    if(avg_wait > 10) cnt[3]++;
    else              cnt[3] = 0;
    if(cnt[3] >= 1){
        cur_boost = clamp(cur_boost - 10, 10, 500);
        fired |= 0x08;
    }

    /* Rule 5: phase change -- immediate; symmetric inc/dec every epoch */
    if(phase_change){
        cur_af = clamp(cur_af + 10, cur_as + 1, 90);
        fired |= 0x10;
    } else {
        cur_af = clamp(cur_af - 5, cur_as + 1, 90);
    }

    /* Rule 6: SJF abuse -- age faster so wait_ticks overrides burst score */
    if(abuse_detected) cnt[5]++;
    else               cnt[5] = 0;
    if(cnt[5] >= 2){
        cur_aging = clamp(cur_aging - 3, 1, 100);
        fired |= 0x20;
    }

    /* Enforce queue monotonicity: q0 <= q1 <= q2 */
    if(cur_q1 < cur_q0) cur_q1 = cur_q0;
    if(cur_q2 < cur_q1) cur_q2 = cur_q1;

    return fired;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int
main(void)
{
    struct schedstat stats[NPROC];
    int n;
    int avg_wait, queue_pressure, workload_type, phase_change, abuse_detected;
    int alert, fired;

    if(register_agent() < 0){
        printf(2, "sagent: register_agent failed (another agent running?)\n");
        exit();
    }

    for(;;){
        /* 1. OBSERVE */
        n = getschedstats(stats, NPROC);
        if(n <= 0){
            sleep(EPOCH_TICKS);
            continue;
        }

        /* sched_alert is stored in stats[0].sched_alert by kernel */
        alert = stats[0].sched_alert;

        /* 2. DECIDE -- compute signals */
        avg_wait       = compute_avg_wait(stats, n);
        queue_pressure = compute_queue_pressure(stats, n);
        workload_type  = compute_workload_type(stats, n);
        phase_change   = detect_phase_change(stats, n);
        abuse_detected = detect_sjf_abuse(stats, n);

        /* 3. DECIDE -- apply rules */
        fired = decide_and_update(avg_wait, queue_pressure, workload_type,
                                  phase_change, abuse_detected);

        /* 4. ACTUATE */
        {
            struct sched_update upd;
            upd.q0_quantum     = cur_q0;
            upd.q1_quantum     = cur_q1;
            upd.q2_quantum     = cur_q2;
            upd.boost_interval = cur_boost;
            upd.ema_alpha_fast = cur_af;
            upd.ema_alpha_slow = cur_as;
            upd.aging_factor   = cur_aging;
            setschedparam(&upd);
        }

        /* 5. LOG -- one line per epoch: signals, fired rules, new params */
        epoch++;
        printf(1, "sagent[E=%d]: wait=%d qp=%d wt=%d pc=%d ab=%d |",
               epoch, avg_wait, queue_pressure, workload_type,
               phase_change, abuse_detected);
        if(fired == 0){
            printf(1, " (none)");
        } else {
            if(fired & 0x01) printf(1, " R1");
            if(fired & 0x02) printf(1, " R2");
            if(fired & 0x04) printf(1, " R3");
            if(fired & 0x08) printf(1, " R4");
            if(fired & 0x10) printf(1, " R5");
            if(fired & 0x20) printf(1, " R6");
        }
        printf(1, " | q0=%d q1=%d q2=%d bst=%d af=%d as=%d age=%d\n",
               cur_q0, cur_q1, cur_q2, cur_boost, cur_af, cur_as, cur_aging);

        /* 6. SLEEP -- shorter poll when kernel has flagged starvation */
        if(alert)
            sleep(ALERT_SLEEP);
        else
            sleep(EPOCH_TICKS);
    }
}
