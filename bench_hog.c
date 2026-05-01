/*
 * bench_hog.c -- Long-running CPU hog for MLFQ contention experiments.
 * Runs 100 * 500M arithmetic iterations; quickly demotes to Q2 under MLFQ.
 * Prints nothing until completion so it saturates the CPU silently
 * while bench_interactive (or any other workload) runs in the foreground.
 *
 * Run 3 copies in the background before bench_interactive:
 *   xv6$ bench_hog &
 *   xv6$ bench_hog &
 *   xv6$ bench_hog &
 *   xv6$ bench_interactive
 *
 * Inner loop stays under INT_MAX (2.1B) to avoid signed overflow.
 */
#include "types.h"
#include "user.h"

int
main(void)
{
    int r, i, sum;

    sum = 0;
    for(r = 0; r < 100; r++)
        for(i = 0; i < 500000000; i++)
            sum += i;

    printf(1, "bench_hog: done sum=%d\n", sum);
    exit();
}
