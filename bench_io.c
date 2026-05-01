/*
 * bench_io.c -- I/O-bound benchmark for MLFQ evaluation.
 * Sleeps repeatedly to simulate I/O waits; process stays in Q0/Q1.
 * Prints turnaround and response time ticks on exit.
 */
#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    int i, start, end, response;
    struct schedstat stats[64];
    int n, mypid;

    start = uptime();
    mypid = getpid();

    for(i = 0; i < 200; i++)
        sleep(1);
    end = uptime();

    /* Response time: first_run_tick - start */
    response = 0;
    n = getschedstats(stats, 64);
    for(i = 0; i < n; i++){
        if(stats[i].pid == mypid && stats[i].first_run_tick > 0){
            response = stats[i].first_run_tick - stats[i].create_tick;
            break;
        }
    }

    printf(1, "bench_io: turnaround=%d response=%d\n", end - start, response);
    exit();
}
