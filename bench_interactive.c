/*
 * bench_interactive.c -- Short interactive task for MLFQ priority evaluation.
 * Runs 1M arithmetic iterations; stays in Q0 under MLFQ because the burst is
 * short enough that the quantum never expires before the work is done.
 * Under RR it must wait its turn behind any CPU hogs already running.
 *
 * Usage (run AFTER starting background hogs):
 *   xv6$ bench_hog & ; bench_hog & ; bench_hog & ; bench_interactive
 *
 * Key metric: response time (first_run_tick - create_tick).
 * Under MLFQ, hogs are in Q2 when this starts, so response = 0-1 ticks.
 * Under RR, this waits behind the 3 hogs, so response = 3+ ticks.
 */
#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    int i, sum, start, end, response;
    struct schedstat stats[64];
    int n, mypid;

    sum = 0;
    start = uptime();
    mypid = getpid();

    for(i = 0; i < 1000000; i++)
        sum += i;
    end = uptime();

    response = 0;
    n = getschedstats(stats, 64);
    for(i = 0; i < n; i++){
        if(stats[i].pid == mypid && stats[i].first_run_tick > 0){
            response = stats[i].first_run_tick - stats[i].create_tick;
            break;
        }
    }

    printf(1, "bench_interactive: turnaround=%d response=%d sum=%d\n",
           end - start, response, sum);
    exit();
}
