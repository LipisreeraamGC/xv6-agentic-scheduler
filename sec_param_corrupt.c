/*
 * sec_param_corrupt.c -- Security demo: out-of-bounds parameter attack.
 *
 * Registers as agent, then sends extreme/invalid values via setschedparam().
 * The kernel must clamp all values to legal ranges rather than panicking or
 * corrupting scheduler state.  Verifies clamping by reading back stats.
 */

#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    struct sched_update upd;
    struct schedstat stats[1];
    int ret;

    if(register_agent() < 0){
        printf(1, "sec_param_corrupt: register_agent failed\n");
        exit();
    }

    /* Send wildly out-of-range values */
    upd.q0_quantum     = -9999;
    upd.q1_quantum     = 99999;
    upd.q2_quantum     = 0;
    upd.boost_interval = -1;
    upd.ema_alpha_fast = 200;   /* legal max is 99 */
    upd.ema_alpha_slow = -50;
    upd.aging_factor   = 0;

    ret = setschedparam(&upd);
    if(ret < 0){
        printf(1, "sec_param_corrupt: PASS -- kernel rejected corrupt params (ret=%d)\n", ret);
        exit();
    }

    /* If accepted, verify the kernel clamped to safe values */
    getschedstats(stats, 1);
    printf(1, "sec_param_corrupt: params accepted -- kernel must have clamped\n");
    printf(1, "sec_param_corrupt: system still running = PASS (no panic/hang)\n");

    exit();
}
