/*
 * bench_mixed.c -- Mixed workload benchmark for MLFQ evaluation.
 * Spawns 6 CPU-bound and 4 I/O-bound children, waits for all 10.
 * Measures total turnaround ticks from parent's perspective.
 */
#include "types.h"
#include "user.h"

/* CPU hog: 100M iterations to drive process into Q2 (~60 ticks each) */
static void
cpu_child(void)
{
    int i, sum;
    sum = 0;
    for(i = 0; i < 100000000; i++)
        sum += i;
    exit();
}

/* I/O child: sleeps repeatedly to stay in Q0 */
static void
io_child(void)
{
    int i;
    for(i = 0; i < 200; i++)
        sleep(1);
    exit();
}

/* 6 CPU + 4 IO children: drives queue_pressure >60% to
 * trigger sagent R1/R4 rules during evaluation */
int
main(void)
{
    int i, start, end;

    start = uptime();

    for(i = 0; i < 6; i++){
        if(fork() == 0)
            cpu_child();
    }
    for(i = 0; i < 4; i++){
        if(fork() == 0)
            io_child();
    }

    for(i = 0; i < 10; i++)
        wait();

    end = uptime();
    printf(1, "bench_mixed: all done ticks=%d\n", end - start);
    exit();
}
