/*
 * config_mlfq.c -- Restore scheduler to MLFQ+SJF mode with default params.
 * Registers as agent, resets all params to defaults, sets rr_mode=0.
 * Run before the fixed-MLFQ evaluation runs (without sagent).
 */

#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    struct sched_update upd;

    if(register_agent() < 0){
        printf(1, "config_mlfq: register_agent failed\n");
        exit();
    }

    upd.q0_quantum     = 4;
    upd.q1_quantum     = 6;
    upd.q2_quantum     = 8;
    upd.boost_interval = 500;
    upd.ema_alpha_fast = 50;
    upd.ema_alpha_slow = 10;
    upd.aging_factor   = 50;
    upd.rr_mode        = 0;

    if(setschedparam(&upd) < 0){
        printf(1, "config_mlfq: setschedparam failed\n");
        exit();
    }

    printf(1, "config_mlfq: scheduler set to MLFQ+SJF mode (default params)\n");
    exit();
}
