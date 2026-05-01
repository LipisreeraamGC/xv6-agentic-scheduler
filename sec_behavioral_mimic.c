/*
 * sec_behavioral_mimic.c -- Security demo: CPU-bound process mimics I/O behavior.
 *
 * A CPU-heavy process interleaves compute bursts with sleep(1) calls to make
 * the scheduler's EMA think it is I/O-bound.  The agent's Rule 2 would then
 * mistakenly increase q0 quantum to reward it, unfairly boosting its throughput
 * at the expense of genuinely I/O-bound processes.
 *
 * Compared to sec_sjf_abuse: that demo targets SJF priority; this demo targets
 * the MLFQ workload-type classifier (workload_type signal in sagent).
 */

#include "types.h"
#include "user.h"

#define BURN_ITERS  600000
#define ROUNDS      40

int
main(void)
{
    int i, j;
    volatile int sink;

    printf(1, "sec_behavioral_mimic: mimicking I/O-bound behavior for %d rounds\n",
           ROUNDS);

    sink = 0;
    for(i = 0; i < ROUNDS; i++){
        /* heavy compute -- genuinely CPU-bound work */
        for(j = 0; j < BURN_ITERS; j++)
            sink += j * i;

        /* fake I/O pause -- tricks workload_type signal into high yield ratio */
        sleep(1);
    }

    printf(1, "sec_behavioral_mimic: done (sink=%d)\n", sink);
    exit();
}
