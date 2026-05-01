#include "types.h"
#include "x86.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;
  int signum;

  if(argint(0, &pid) < 0)
    return -1;
  if(argint(1, &signum) < 0)
    return -1;
  return kill(pid, signum);
}

int
sys_alarm(void) {
  int secs;
  if(argint(0, &secs) < 0)
    return -1;

  // Convert seconds to timer ticks.
  // xv6's timer fires ~100 times/sec (HZ=100), so 1 sec = 100 ticks.
  // Verify the actual HZ value in your trap.c or param.h — check
  // the IRQ_TIMER handler to see how ticks is incremented.
  proc->alarm_ticks    = secs * 100;
  proc->alarm_countdown = (secs + 1) * 100;

  return 0;
}

int
sys_signal(void) {
  int signum;
  void (*handler)(int) = 0;

  if(argint(0, &signum) < 0)
    return -1;
  if(argaddr(1, (addr_t *)&handler) < 0)
    return -1;
  if(signum < 0 || signum >= 32)
    return -1;

  // Delegate to signal() in proc.c — it prints the debug line and stores.
  signal(signum, handler);
  return 0;
}

int
sys_sigret(void) {
  cprintf("In sys_sigret\n");
  // Restore the trapframe saved before the signal handler ran.
  // syscall_trapret will then use tf_backup.rcx as the return RIP
  // (via sysretq) and tf_backup.rsp as the user stack pointer.
  *proc->tf = proc->tf_backup;
  return 0;
}

int
sys_fgproc(void) {
  extern int fgpid;
  fgpid = proc->pid;
  cprintf("Set process to FG\n");
  return 0;
}

int
sys_getpid(void)
{
  return proc->pid;
}

addr_t
sys_sbrk(void)
{
  addr_t addr;
  addr_t n;

  argaddr(0, &n);
  addr = proc->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(proc->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

/*
 * sys_getschedstats(struct schedstat *buf, int maxproc)
 * Returns number of entries written, -1 on error.
 * Validates the FULL buffer (maxproc entries) -- fixes spec undervalidation.
 */
int
sys_getschedstats(void)
{
  addr_t buf_addr, buf_end;
  int maxproc;
  struct schedstat *buf;

  if(argaddr(0, &buf_addr) < 0) return -1;
  if(argint(1, &maxproc)   < 0) return -1;
  if(maxproc <= 0 || maxproc > NPROC) return -1;

  buf_end = buf_addr + (addr_t)maxproc * sizeof(struct schedstat);
  if(buf_addr < PGSIZE || buf_addr >= proc->sz || buf_end > proc->sz)
    return -1;

  buf = (struct schedstat *)buf_addr;
  return kern_getschedstats(buf, maxproc);
}

/*
 * sys_setschedparam(q0,q1,q2,boost,alpha_fast,alpha_slow,aging)
 * Only the registered agent (schedp.agent_pid) may call this.
 * Improvements: enforces alpha_slow < alpha_fast and q0<=q1<=q2.
 */
int
sys_setschedparam(void)
{
  addr_t upd_addr;
  struct sched_update *upd;

  if(schedp.agent_pid == 0 || proc->pid != schedp.agent_pid)
    return -1;

  if(argaddr(0, &upd_addr) < 0) return -1;
  if(upd_addr < PGSIZE || upd_addr + sizeof(struct sched_update) > proc->sz)
    return -1;

  upd = (struct sched_update *)upd_addr;

  if(upd->q0_quantum < 1 || upd->q0_quantum > 10)           return -1;
  if(upd->q1_quantum < 1 || upd->q1_quantum > 20)           return -1;
  if(upd->q2_quantum < 1 || upd->q2_quantum > 40)           return -1;
  if(upd->boost_interval < 10 || upd->boost_interval > 500) return -1;
  if(upd->ema_alpha_fast < 1 || upd->ema_alpha_fast > 90)   return -1;
  if(upd->ema_alpha_slow < 1 || upd->ema_alpha_slow > 50)   return -1;
  if(upd->aging_factor < 1 || upd->aging_factor > 100)      return -1;
  if(upd->ema_alpha_slow >= upd->ema_alpha_fast)             return -1;
  if(upd->q1_quantum < upd->q0_quantum || upd->q2_quantum < upd->q1_quantum) return -1;
  if(upd->rr_mode != 0 && upd->rr_mode != 1)                return -1;

  acquire(&schedp.lock);
  schedp.q0_quantum     = upd->q0_quantum;
  schedp.q1_quantum     = upd->q1_quantum;
  schedp.q2_quantum     = upd->q2_quantum;
  schedp.boost_interval = upd->boost_interval;
  schedp.ema_alpha_fast = upd->ema_alpha_fast;
  schedp.ema_alpha_slow = upd->ema_alpha_slow;
  schedp.aging_factor   = upd->aging_factor;
  schedp.sched_alert    = 0;
  schedp.rr_mode        = upd->rr_mode;
  release(&schedp.lock);
  return 0;
}

/*
 * sys_register_agent(void)
 * Registers caller as the scheduler agent. One-shot; TOCTOU-safe.
 * Pins agent to Q0 so it is never demoted or starved.
 */
int
sys_register_agent(void)
{
  if(schedp.agent_pid != 0)
    return -1;

  acquire(&schedp.lock);
  if(schedp.agent_pid != 0) {
    release(&schedp.lock);
    return -1;
  }
  schedp.agent_pid = proc->pid;
  release(&schedp.lock);

  kern_pin_agent();
  return 0;
}
