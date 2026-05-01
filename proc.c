#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

/* Tunable scheduler parameters -- see proc.h for field descriptions */
struct sched_params schedp;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void syscall_trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&schedp.lock, "schedp");
  schedp.q0_quantum     = 4;
  schedp.q1_quantum     = 6;
  schedp.q2_quantum     = 8;
  schedp.boost_interval = 500;
  schedp.ema_alpha_fast = 50;
  schedp.ema_alpha_slow = 10;
  schedp.aging_factor   = 50;
  schedp.agent_pid      = 0;
  schedp.sched_alert    = 0;
  schedp.rr_mode        = 0;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->signal_pending = 0;
  p->alarm_ticks = 0;
  p->alarm_countdown = 0;
  memset(p->sig_disp, 0, sizeof(p->sig_disp));
  p->pid = nextpid++;

  /* MLFQ+SJF fields: all new processes start in Q0 */
  p->queue_level      = 0;
  p->ticks_remaining  = 1;   /* Q0 quantum; reset by scheduler before each run */
  p->wait_ticks       = 0;
  p->burst_ema_fast   = 100; /* scaled by 100: initial estimate = 1 tick */
  p->burst_ema_slow   = 100;
  p->burst_start_tick = 0;
  p->yield_count      = 0;
  p->preempt_count    = 0;
  p->burst_variance   = 0;
  p->page_fault_count = 0;
  p->effective_score  = 0;
  p->first_run_tick   = 0;
  p->create_tick      = ticks;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= sizeof(addr_t);
  *(addr_t*)sp = (addr_t)syscall_trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (addr_t)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");

  inituvm(p->pgdir, _binary_initcode_start,
          (addr_t)_binary_initcode_size);
  p->sz = PGSIZE * 2;
  memset(p->tf, 0, sizeof(*p->tf));

  p->tf->r11 = FL_IF;  // with SYSRET, EFLAGS is in R11
  p->tf->rsp = p->sz;
  p->tf->rcx = PGSIZE;  // with SYSRET, RIP is in RCX

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  __sync_synchronize();
  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int64 n)
{
  addr_t sz;

  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}
//PAGEBREAK!

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %rax so that fork returns 0 in the child.
  np->tf->rax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  pid = np->pid;

  __sync_synchronize();
  np->state = RUNNABLE;

  return pid;
}

//PAGEBREAK!
// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  /* Release agent slot if this process held it */
  acquire(&schedp.lock);
  if(schedp.agent_pid == proc->pid)
    schedp.agent_pid = 0;
  release(&schedp.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

//PAGEBREAK!
// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct proc *best;
  int q;
  int nothing_found;
  int snap_boost, snap_q0, snap_q1, snap_q2, snap_aging, snap_rr;
  static uint last_boost_tick = 0;

  for(;;) {
    sti();
    acquire(&ptable.lock);
    nothing_found = 1;

    /* Snapshot schedp once per pass under its lock so the scheduler
     * sees a coherent parameter set even if the agent updates mid-pass. */
    acquire(&schedp.lock);
    snap_boost  = schedp.boost_interval;
    snap_q0     = schedp.q0_quantum;
    snap_q1     = schedp.q1_quantum;
    snap_q2     = schedp.q2_quantum;
    snap_aging  = schedp.aging_factor;
    snap_rr     = schedp.rr_mode;
    release(&schedp.lock);

    /*
     * Step 1: Priority boost.
     */
    if(ticks - last_boost_tick >= (uint)snap_boost) {
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if(p->state == UNUSED) continue;
        p->queue_level     = 0;
        p->ticks_remaining = snap_q0;
        p->wait_ticks      = 0;
      }
      last_boost_tick = ticks;
    }

    /*
     * Step 2: Age all RUNNABLE processes.
     * Each pass through the scheduler loop counts as one scheduling
     * decision; processes that keep losing the SJF contest accumulate
     * wait_ticks and eventually win via the aging term.
     */
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if(p->state != RUNNABLE) continue;
      p->wait_ticks++;
      /* Alert the agent when any process is starving */
      if(p->wait_ticks > 100 && schedp.sched_alert == 0) {  /* sched_alert written under its own lock below */
        acquire(&schedp.lock);
        schedp.sched_alert = 1;
        release(&schedp.lock);
      }
    }

    /*
     * Step 3: Process selection.
     * snap_rr=1: true round-robin baseline -- rotating start index avoids
     *            PID bias (lower-numbered procs would always win a fixed scan).
     * snap_rr=0: MLFQ+SJF -- check queues in priority order, pick lowest score.
     */
    best = 0;
    if(snap_rr) {
      /* True round-robin: advance start index after each pick */
      static int rr_next = 0;
      int i;
      for(i = 0; i < NPROC; i++) {
        p = &ptable.proc[(rr_next + i) % NPROC];
        if(p->state == RUNNABLE) {
          best = p;
          rr_next = (int)(p - ptable.proc + 1) % NPROC;
          break;
        }
      }
    } else {
      for(q = 0; q <= 2; q++) {
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
          if(p->state != RUNNABLE) continue;
          if(p->queue_level != q) continue;
          /* Lower score = higher priority */
          p->effective_score = p->burst_ema_fast / 100
                               - (p->wait_ticks / snap_aging);
          if(best == 0 || p->effective_score < best->effective_score)
            best = p;
        }
        if(best != 0) break; /* found something in this queue -- stop */
      }
    }

    /*
     * Step 4: Dispatch best process.
     */
    if(best != 0) {
      nothing_found = 0;
      best->wait_ticks      = 0;      /* reset wait counter on dispatch */
      best->burst_start_tick = ticks; /* record burst start for EMA update */

      /* Record first-ever scheduling time for response time measurement */
      if(best->first_run_tick == 0)
        best->first_run_tick = ticks;

      /* Assign a fresh quantum from the consistent snapshot */
      if(best->queue_level == 0)
        best->ticks_remaining = snap_q0;
      else if(best->queue_level == 1)
        best->ticks_remaining = snap_q1;
      else
        best->ticks_remaining = snap_q2;

      proc = best;
      switchuvm(best);
      best->state = RUNNING;
      swtch(&cpu->scheduler, best->context);
      switchkvm();
      proc = 0;
    }

    release(&ptable.lock);

    /* Step 5: Halt when nothing is runnable -- preserves original hlt() path */
    if(nothing_found)
      hlt();
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;


  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  /*DOC: yieldlock*/
  proc->yield_count++;    /* count voluntary context switches for workload detection */
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

//PAGEBREAK!
// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

void
check_alarms(void)
{
  struct proc *p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->alarm_ticks > 0) {
      p->alarm_countdown--;
      if(p->alarm_countdown <= 0) {
        cprintf("got a kill for %d signal %d\n", p->pid, SIGALRM);
        p->signal_pending = SIGALRM;
        p->alarm_ticks = 0;
        p->alarm_countdown = 0;
        if(p->state == SLEEPING)
          p->state = RUNNABLE;
      }
    }
  }
  release(&ptable.lock);
}


void
alarm(int secs) {
  // actual work is in sys_alarm() — this kernel stub is unused
  (void)secs;
}

void
signal(int signum, void (*handler)(int)) {
  cprintf("In signal(), with number %d and handler %p\n",
          signum, (addr_t)handler);
  if(signum >= 0 && signum < 32)
    proc->sig_disp[signum] = (uint64)(addr_t)handler;
}

void
check_signals()
{
  if(proc == 0)
    return;

  // Hard kill (e.g. from a trap fault)
  if(proc->killed)
    exit();

  if(proc->signal_pending == 0)
    return;

  int signum    = (int)proc->signal_pending;
  uint64 disp   = proc->sig_disp[signum];

  cprintf("Checking signals, pending is %d\n", signum);

  if(disp == 1){
    // SIG_IGN: discard silently
    proc->signal_pending = 0;
    return;
  }

  if(disp == 0){
    // SIG_DFL: terminate
    cprintf("got a signal %d, exiting\n", signum);
    exit();
  }

  // --- User-space handler ---
  void (*handler)(int) = (void(*)(int))(addr_t)disp;

  // Back up the current trapframe.
  // Normalise rcx to rip so that syscall_trapret (sysretq path)
  // returns to the right user PC after sys_sigret restores the backup.
  proc->tf_backup     = *proc->tf;
  proc->tf_backup.rcx = proc->tf->rip;

  proc->signal_pending = 0;

  // Trampoline machine code (identical to the sigret stub in usys.S):
  //   48 c7 c0 18 00 00 00   movq $24 (SYS_sigret), %rax
  //   49 89 ca               movq %rcx, %r10
  //   0f 05                  syscall
  //   c3                     retq
  static uchar sigret_code[] = {
    0x48, 0xc7, 0xc0, 0x18, 0x00, 0x00, 0x00,
    0x49, 0x89, 0xca,
    0x0f, 0x05,
    0xc3
  };

  addr_t usp = proc->tf->rsp;

  // Push trampoline bytes onto the user stack
  usp -= sizeof(sigret_code);
  usp &= ~(addr_t)0xF;   // 16-byte align
  copyout(proc->pgdir, usp, sigret_code, sizeof(sigret_code));
  addr_t trampoline = usp;

  // Push the trampoline address as the handler's return address
  usp -= 8;
  copyout(proc->pgdir, usp, &trampoline, 8);

  // Redirect execution to the handler
  proc->tf->rsp = usp;              // user %rsp: return addr on top
  proc->tf->rcx = (addr_t)handler;  // sysretq path: RIP ← %rcx
  proc->tf->rip = (addr_t)handler;  // iretq   path: RIP ← tf->rip
  proc->tf->rdi = signum;            // first argument to handler
}

// Signal the process with the given pid and signal
int
kill(int pid, int signum)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->pid == pid){
      cprintf("got a kill for %d signal %d\n", pid, signum);
      p->signal_pending = signum;

      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);

      return 0;
    }
  }
  release(&ptable.lock);

  return -1;
}

// Send a signal to the foreground process (called from consoleintr)
int fgpid = 0;

void
send_fgsig(int signum)
{
  if(fgpid > 0)
    kill(fgpid, signum);
}


/*
 * kern_getschedstats -- kernel helper for sys_getschedstats.
 * Copies a telemetry snapshot of all non-UNUSED processes into buf.
 * Returns the number of entries written.
 * Piggybacks sched_alert on buf[0] so the agent sees starvation alerts
 * in the same call (no extra syscall needed).
 */
int
kern_getschedstats(struct schedstat *buf, int maxproc)
{
  int n;
  struct proc *p;

  n = 0;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC] && n < maxproc; p++) {
    if(p->state == UNUSED) continue;
    buf[n].pid              = p->pid;
    buf[n].state            = (int)p->state;
    buf[n].queue_level      = p->queue_level;
    buf[n].burst_ema_fast   = p->burst_ema_fast;
    buf[n].burst_ema_slow   = p->burst_ema_slow;
    buf[n].wait_ticks       = p->wait_ticks;
    buf[n].yield_count      = p->yield_count;
    buf[n].preempt_count    = p->preempt_count;
    buf[n].burst_variance   = p->burst_variance;
    buf[n].page_fault_count = p->page_fault_count;
    buf[n].effective_score  = p->effective_score;
    buf[n].sched_alert      = (n == 0) ? schedp.sched_alert : 0;
    buf[n].first_run_tick   = p->first_run_tick;
    buf[n].create_tick      = p->create_tick;
    n++;
  }
  release(&ptable.lock);
  return n;
}

/*
 * kern_pin_agent -- pin the current process permanently to Q0.
 * Must hold ptable.lock to write queue_level/ticks_remaining because
 * the scheduler reads them under that same lock.
 */
void
kern_pin_agent(void)
{
  acquire(&ptable.lock);
  proc->queue_level     = 0;
  proc->ticks_remaining = schedp.q0_quantum;
  release(&ptable.lock);
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  addr_t pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getstackpcs((addr_t*)p->context->rbp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
