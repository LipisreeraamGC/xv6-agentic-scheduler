/*
 * config_rr.c -- Switch scheduler to round-robin mode for evaluation baseline.
 * Registers as agent, sets rr_mode=1, then exits.
 * The kernel continues running in RR mode until config_mlfq is run.
 */

#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    struct sched_update upd;

    if(register_agent() < 0){
        printf(1, "config_rr: register_agent failed\n");
        exit();
    }

    /* Valid MLFQ params required by validation -- values don't matter in RR mode */
    upd.q0_quantum     = 1;
    upd.q1_quantum     = 2;
    upd.q2_quantum     = 4;
    upd.boost_interval = 100;
    upd.ema_alpha_fast = 50;
    upd.ema_alpha_slow = 10;
    upd.aging_factor   = 10;
    upd.rr_mode        = 1;

    if(setschedparam(&upd) < 0){
        printf(1, "config_rr: setschedparam failed\n");
        exit();
    }

    printf(1, "config_rr: scheduler set to ROUND-ROBIN mode\n");
    exit();
}
