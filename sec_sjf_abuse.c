/*
 * sec_sjf_abuse.c -- Security demo: SJF gaming via strategic early sleep.
 *
 * A CPU-bound process voluntarily sleeps before its quantum expires,
 * keeping its burst_ema artificially low.  The SJF scorer then ranks it
 * ahead of genuinely short processes.  The agent's Rule 6 (detect_sjf_abuse)
 * should detect low burst_variance + high preempt_count and counter by
 * increasing the aging factor.
 *
 * Note: xv6 has no sys_yield userspace wrapper; sleep(1) is used as the
 * equivalent voluntary-CPU-surrender primitive.
 */

#include "types.h"
#include "user.h"

#define BURN_ITERS  800000
#define ROUNDS      30

int
main(void)
{
    int i, j;
    volatile int sink;

    printf(1, "sec_sjf_abuse: starting -- burn+sleep loop for %d rounds\n", ROUNDS);

    sink = 0;
    for(i = 0; i < ROUNDS; i++){
        /* burn CPU, then sleep before preemption to keep burst EMA low */
        for(j = 0; j < BURN_ITERS; j++)
            sink += j;
        sleep(1); /* strategic voluntary surrender -- keeps each burst short */
    }

    printf(1, "sec_sjf_abuse: done (sink=%d)\n", sink);
    exit();
}
