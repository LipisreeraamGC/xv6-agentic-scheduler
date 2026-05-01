/*
 * attack_priv.c -- Security demo: attempt setschedparam() without
 * being the registered agent. Kernel should reject with -1.
 */
#include "types.h"
#include "user.h"
#include "schedstat.h"

int
main(void)
{
    struct sched_update upd;
    int ret;

    upd.q0_quantum     = 1;
    upd.q1_quantum     = 2;
    upd.q2_quantum     = 4;
    upd.boost_interval = 100;
    upd.ema_alpha_fast = 50;
    upd.ema_alpha_slow = 10;
    upd.aging_factor   = 10;
    upd.rr_mode        = 0;

    printf(1, "[attack_priv] attempting to call setschedparam() directly...\n");

    ret = setschedparam(&upd);

    if(ret < 0){
        printf(1, "setschedparam returned -1 (access denied - not the registered agent)\n");
        printf(1, "[attack_priv] privilege escalation FAILED as expected\n");
    } else {
        printf(1, "[attack_priv] WARNING: setschedparam SUCCEEDED - security breach!\n");
    }

    exit();
}
