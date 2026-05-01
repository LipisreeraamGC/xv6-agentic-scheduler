/*
 * sec_fork_bomb.c -- Security demo: reward gaming via rapid fork/exit.
 *
 * A process exploits MLFQ priority reset on fork: each new child starts at Q0.
 * By forking rapidly and exiting before demotion, the attacker keeps a stream
 * of processes running at the highest priority queue, starving honest processes.
 * Run alongside bench_cpu to observe impact on its turnaround time.
 */

#include "types.h"
#include "user.h"

#define ROUNDS 20

int
main(void)
{
    int i, pid;

    printf(1, "sec_fork_bomb: spawning %d rapid fork/exit children\n", ROUNDS);

    for(i = 0; i < ROUNDS; i++){
        pid = fork();
        if(pid < 0){
            printf(1, "sec_fork_bomb: fork failed at round %d\n", i);
            break;
        }
        if(pid == 0){
            /* child: exit immediately -- never exhausts quantum, stays Q0 */
            exit();
        }
        /* parent: wait and immediately fork again */
        wait();
    }

    printf(1, "sec_fork_bomb: done -- %d short-lived Q0 processes created\n", ROUNDS);
    exit();
}
