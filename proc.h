#include "x86.h"
#include "schedstat.h"
// Per-CPU state
struct cpu {
  uchar id;
  uchar apicid;              // Local APIC ID
  struct context *scheduler; // swtch() here to enter scheduler
  volatile uint started;     // Has the CPU started?
  int ncli;                  // Depth of pushcli nesting.
  int intena;                // Were interrupts enabled before pushcli?
  void *local;               // CPU-local storage; see seginit()
};

extern struct cpu cpus[NCPU];
extern int ncpu;

// Per-CPU variables, holding pointers to the
// current cpu and to the current process.
// The asm suffix tells gcc to use "%gs:(-16)" to refer to cpu
// and "%gs:(-8)" to refer to proc.  seginit sets up the
// %gs segment register so that %gs refers to the memory
// holding those two variables in the local cpu's struct cpu.
// This is similar to how thread-local variables are implemented
// in thread libraries such as Linux pthreads.
extern __thread struct cpu *cpu;
extern __thread struct proc *proc;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %rax, %rcx, etc., because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save rip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  addr_t r15;
  addr_t r14;
  addr_t r13;
  addr_t r12;
  addr_t rbx;
  addr_t rbp;
  addr_t rip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
enum signal { SIGINT=2, SIGKILL=9, SIGALRM=14 };

/*
 * spinlock.h needs struct cpu (defined above) only as a pointer --
 * include it here so struct sched_params can embed a struct spinlock.
 */
#include "spinlock.h"

/*
 * Tunable scheduler parameters -- modified by agent via setschedparam syscall.
 * Separate spinlock from ptable.lock; acquire ptable.lock first if both needed.
 */
struct sched_params {
  struct spinlock lock;
  int q0_quantum;        /* ticks for Q0 processes (default: 1) */
  int q1_quantum;        /* ticks for Q1 processes (default: 2) */
  int q2_quantum;        /* ticks for Q2 processes (default: 4) */
  int boost_interval;    /* ticks between priority boosts (default: 100) */
  int ema_alpha_fast;    /* fast EMA alpha * 100 (default: 50 = 0.5) */
  int ema_alpha_slow;    /* slow EMA alpha * 100 (default: 10 = 0.1) */
  int aging_factor;      /* aging divisor for effective_score (default: 10) */
  int agent_pid;         /* PID of registered agent -- 0 if none */
  int sched_alert;       /* set by kernel when starvation threshold crossed */
  int rr_mode;           /* 1 = plain round-robin (bypass MLFQ+SJF); 0 = MLFQ */
};

extern struct sched_params schedp;

// Per-process state
struct proc {
  addr_t sz;                   // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  struct trapframe tf_backup; 
  enum signal signal_pending;
  int alarm_ticks;       // original interval in ticks (0 = no alarm)
  int alarm_countdown;   // ticks remaining until SIGALRM fires
  uint64 sig_disp[32];      // sig_disp[signum]: 0=SIG_DFL (kill), 1=SIG_IGN

  /* MLFQ+SJF scheduler fields -- added for term project */
  int queue_level;       /* MLFQ queue: 0=highest, 1=medium, 2=lowest */
  int ticks_remaining;   /* ticks left in current quantum; <= 0 triggers yield */
  int wait_ticks;        /* scheduling decisions spent RUNNABLE but not picked */
  int burst_ema_fast;    /* fast burst EMA: alpha=50, scaled by 100 */
  int burst_ema_slow;    /* slow burst EMA: alpha=10, scaled by 100 */
  int burst_start_tick;  /* tick when current CPU burst started */
  int yield_count;       /* voluntary context switches (process called yield) */
  int preempt_count;     /* forced context switches (quantum exhausted) */
  int burst_variance;    /* |burst_ema_fast - burst_ema_slow| */
  int page_fault_count;  /* page faults since last reset */
  int effective_score;   /* SJF score with aging: burst_ema_fast/100 - wait/aging */
  int first_run_tick;    /* uptime tick when process was first scheduled; 0 = never */
  int create_tick;       /* uptime tick when allocproc() ran (job arrival time) */
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
