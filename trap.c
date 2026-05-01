#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
uint *idt;
extern addr_t vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

static void
mkgate(uint *idt, uint n, addr_t kva, uint pl)
{
  uint64 addr = (uint64) kva;

  n *= 4;
  idt[n+0] = (addr & 0xFFFF) | (KERNEL_CS << 16);
  idt[n+1] = (addr & 0xFFFF0000) | 0x8E00 | ((pl & 3) << 13);
  idt[n+2] = addr >> 32;
  idt[n+3] = 0;
}

void idtinit(void)
{
  lidt((void*) idt, PGSIZE);
}


void tvinit(void)
{
  int n;
  idt = (uint*) kalloc();
  memset(idt, 0, PGSIZE);

  for (n = 0; n < 256; n++)
    mkgate(idt, n, vectors[n], 0);
}

int from_bcd(int code) {
  return (code>>4)*10+(code&0xf);
}

void print_rtc_time() {
      outb(0x70, 0x00);  // Request the seconds
      int secs = from_bcd(inb(0x71));
      outb(0x70, 0x02);  // Request the mins
      int mins = from_bcd(inb(0x71));
      outb(0x70, 0x04);  // Request the hours
      int hours = from_bcd(inb(0x71));

      cprintf("%d:%d:%d\n",hours,mins,secs);
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
  if(cpunum() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
    check_alarms();
  }
  lapiceoi();

  /*
   * MLFQ+SJF telemetry update for the currently running process.
   * Runs on every CPU at every timer tick (not just CPU 0) so that
   * each CPU tracks its own running process's quantum correctly.
   * ptable.lock is NOT held here (acquire() disables interrupts, so
   * the timer IRQ fires only when no spinlock is held by this CPU).
   */
  if(proc != 0 && proc->state == RUNNING) {
    proc->ticks_remaining--;
    if(proc->ticks_remaining <= 0) {
      int burst_len = ticks - proc->burst_start_tick;

      /*
       * EMA update (integer, scaled by 100).
       * Formula: new = old + alpha * (burst*100 - old) / 100
       * This is the fixed-point equivalent of: new = (1-a)*old + a*burst*100
       */
      proc->burst_ema_fast = proc->burst_ema_fast +
        (schedp.ema_alpha_fast * (burst_len * 100 - proc->burst_ema_fast)) / 100;
      proc->burst_ema_slow = proc->burst_ema_slow +
        (schedp.ema_alpha_slow * (burst_len * 100 - proc->burst_ema_slow)) / 100;

      /* Cap to prevent int overflow on very long bursts (>50 ticks scaled) */
      if(proc->burst_ema_fast > 5000) proc->burst_ema_fast = 5000;
      if(proc->burst_ema_slow > 5000) proc->burst_ema_slow = 5000;
      if(proc->burst_ema_fast < 0)    proc->burst_ema_fast = 0;
      if(proc->burst_ema_slow < 0)    proc->burst_ema_slow = 0;

      /* Variance proxy: absolute difference between fast and slow EMA */
      proc->burst_variance = proc->burst_ema_fast - proc->burst_ema_slow;
      if(proc->burst_variance < 0) proc->burst_variance = -proc->burst_variance;

      proc->preempt_count++;

      /*
       * MLFQ demotion: only move down, never below Q2.
       * Never demote the registered agent -- it must stay in Q0
       * to remain responsive (spec section 10.2).
       */
      if(proc->queue_level < 2 &&
         (schedp.agent_pid == 0 || proc->pid != schedp.agent_pid))
        proc->queue_level++;

      /*
       * Leave ticks_remaining <= 0.  The scheduler resets it to the
       * correct quantum when the process is next dispatched.
       * The yield() at the bottom of trap() will preempt this process
       * because ticks_remaining <= 0.
       */
    }
  }
  break;

  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %p:%p\n",
            cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  case T_PGFLT:
    /* Count page faults per process for workload characterization */
    if(proc != 0)
      proc->page_fault_count++;
    /* FALLTHROUGH to default handler -- user process gets killed */

  //PAGEBREAK: 13
  default:
    if(proc == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d rip %p (cr2=0x%p)\n",
              tf->trapno, cpunum(), tf->rip, rcr2());
      if (proc)
        cprintf("proc id: %d\n", proc->pid);
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%p addr 0x%p--kill proc\n",
            proc->pid, proc->name, tf->trapno, tf->err, cpunum(), tf->rip,
            rcr2());
    proc->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();

  /*
   * MLFQ preemption: only yield when the quantum is exhausted.
   * ticks_remaining is decremented above in the timer IRQ handler;
   * when it drops to <= 0 the process has used its full quantum and
   * should be preempted and demoted.  Processes that voluntarily call
   * yield() (I/O-bound) never reach this path with ticks_remaining <= 0,
   * so they stay in their current queue level.
   */
  if(proc && proc->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER) {
    if(proc->ticks_remaining <= 0)
      yield();
  }

  // Check if the process has been killed since we yielded
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();
  // deliver pending signals when returning to user space
  if(proc && (tf->cs&3) == DPL_USER)
    check_signals();
}
