#ifndef _KERN_LIB_THREAD_H_
#define _KERN_LIB_THREAD_H_

#ifdef _KERN_

#include "signal.h"

#define SCHED_SLICE 5

typedef enum {TSTATE_READY=0, TSTATE_RUN, TSTATE_SLEEP, TSTATE_DEAD} t_state;

struct thread {
    uint32_t tid;           // thread ID
    t_state state;          // thread state
    uint32_t *stack;        // stack pointer
    uint32_t *esp;          // saved stack pointer
    uint32_t *ebp;          // saved base pointer
    uint32_t eip;           // saved instruction pointer
    uint32_t eflags;        // saved flags
    uint32_t *page_dir;     // page directory
    struct thread *next;    // next thread in list
    struct sig_state sigstate;  // signal state
};

#endif /* _KERN_ */

#endif /* !_KERN_LIB_THREAD_H_ */
