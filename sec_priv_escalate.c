/*
 * sec_priv_escalate.c -- Security demo: privilege escalation attempt.
 *
 * A non-agent process calls setschedparam() directly.  The kernel checks
 * schedp.agent_pid and rejects any call from a process that has not
 * registered via register_agent().  Expected result: setschedparam returns -1.
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
    upd.q1_quantum     = 1;
    upd.q2_quantum     = 1;
    upd.boost_interval = 1;
    upd.ema_alpha_fast = 90;
    upd.ema_alpha_slow = 80;
    upd.aging_factor   = 1;

    ret = setschedparam(&upd);
    if(ret < 0)
        printf(1, "sec_priv_escalate: PASS -- setschedparam rejected non-agent (ret=%d)\n", ret);
    else
        printf(1, "sec_priv_escalate: FAIL -- setschedparam accepted non-agent (ret=%d)\n", ret);

    exit();
}
