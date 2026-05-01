/*
 * bench_cpu.c -- CPU-bound benchmark for MLFQ evaluation.
 * Runs a tight arithmetic loop to drive the process into Q2.
 * Prints turnaround and response time ticks on exit.
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

    for(i = 0; i < 100000000; i++)
        sum += i;
    end = uptime();

    /* Response time: first_run_tick - start (0 if never scheduled, shouldn't happen) */
    response = 0;
    n = getschedstats(stats, 64);
    for(i = 0; i < n; i++){
        if(stats[i].pid == mypid && stats[i].first_run_tick > 0){
            response = stats[i].first_run_tick - stats[i].create_tick;
            break;
        }
    }

    printf(1, "bench_cpu: turnaround=%d response=%d sum=%d\n",
           end - start, response, sum);
    exit();
}
