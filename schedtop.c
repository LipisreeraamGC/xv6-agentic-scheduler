/*
 * schedtop.c -- Live MLFQ+SJF scheduler monitor.
 * Polls getschedstats() every 100 ticks and prints a per-process table.
 * Kill with Ctrl-C.
 */
#include "types.h"
#include "user.h"
#include "schedstat.h"

#define NPROC 64

int
main(void)
{
    struct schedstat stats[NPROC];
    int n, i;

    for(;;){
        n = getschedstats(stats, NPROC);

        printf(1, "\n--- SCHEDULER STATE (n=%d) ---\n", n);
        if(n > 0 && stats[0].sched_alert)
            printf(1, "*** STARVATION ALERT ***\n");

        printf(1, "PID  Q  EMA_F EMA_S WAIT YIELD PRMT PF  SCORE\n");
        for(i = 0; i < n; i++){
            printf(1, "%d  %d  %d  %d  %d  %d  %d  %d  %d\n",
                stats[i].pid,
                stats[i].queue_level,
                stats[i].burst_ema_fast / 100,
                stats[i].burst_ema_slow / 100,
                stats[i].wait_ticks,
                stats[i].yield_count,
                stats[i].preempt_count,
                stats[i].page_fault_count,
                stats[i].effective_score);
        }
        sleep(100);
    }
}
