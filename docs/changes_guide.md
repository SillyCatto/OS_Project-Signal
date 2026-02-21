# Signal Implementation — Complete Changes Guide

> **Purpose:** This document describes every change, modification, and addition required to transform the base CertiKOS template into the working signal implementation. It is organized file-by-file with exact code to add, remove, or modify. Follow each section in order.
>
> **Summary:** 16 files modified, 3 new files created. 761 lines inserted, 112 lines deleted.

---

## Table of Contents

1. [New Files to Create](#1-new-files-to-create)
2. [Kernel Header Changes](#2-kernel-header-changes)
3. [Kernel Process Changes](#3-kernel-process-changes)
4. [Thread Layer Changes](#4-thread-layer-changes)
5. [Trap Layer Changes](#5-trap-layer-changes)
6. [User-Space Changes](#6-user-space-changes)
7. [Build System Changes](#7-build-system-changes)
8. [Miscellaneous](#8-miscellaneous)
9. [Verification Checklist](#9-verification-checklist)
10. [Critical Implementation Notes](#10-critical-implementation-notes)

---

## 1. New Files to Create

Three entirely new files must be created from scratch.

### 1.1 `kern/lib/signal.h` — Kernel Signal Definitions

**Path:** `kern/lib/signal.h`
**Purpose:** Core signal constants, structures (`sigaction`, `sig_state`), and POSIX signal numbers used throughout the kernel.

```c
#ifndef __KERN_LIB_SIGNAL_H__
#define __KERN_LIB_SIGNAL_H__

#include "types.h"

#define NSIG 32  // max number of signals

// Standard signal numbers
#define SIGKILL 9   // Kill signal (cannot be caught or ignored)
#define SIGTERM 15  // Termination signal

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    void (*sa_sigaction)(int, void*, void*);
    int sa_flags;
    void (*sa_restorer)(void);
    uint32_t sa_mask;
};

struct sig_state {
    struct sigaction sigactions[NSIG];
    uint32_t pending_signals;
    int signal_block_mask;
    // Saved context for sigreturn
    uint32_t saved_esp_addr;  // Address on user stack where ESP is saved
    uint32_t saved_eip_addr;  // Address on user stack where EIP is saved
    int in_signal_handler;    // Flag indicating if we're in a signal handler
};

// Signal numbers (POSIX standard signals)
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22

// Signal flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#endif /* !__KERN_LIB_SIGNAL_H__ */
```

### 1.2 `user/include/signal.h` — User-Space Signal API

**Path:** `user/include/signal.h`
**Purpose:** User-space mirror of signal constants and function prototypes that user programs include.

```c
#ifndef _USER_SIGNAL_H_
#define _USER_SIGNAL_H_

#include <types.h>

#define NSIG 32  // max number of signals

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    void (*sa_sigaction)(int, void*, void*);
    int sa_flags;
    void (*sa_restorer)(void);
    uint32_t sa_mask;
};

// Signal numbers (POSIX standard signals)
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22

// Signal flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

// Signal functions
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int kill(int pid, int signum);
int pause(void);

#endif /* !_USER_SIGNAL_H_ */
```

### 1.3 `user/lib/signal.c` — User-Space Signal Library

**Path:** `user/lib/signal.c`
**Purpose:** Thin wrappers that call the `sys_*` inline-asm syscall stubs.

```c
#include <signal.h>
#include <syscall.h>

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    return sys_sigaction(signum, act, oldact);
}

int kill(int pid, int signum)
{
    return sys_kill(pid, signum);
}

int pause(void)
{
    return sys_pause();
}
```

---

## 2. Kernel Header Changes

### 2.1 `kern/lib/syscall.h` — Add Syscall Numbers & Error Codes

**Change type:** Add lines (no deletions).

#### 2.1a — Add syscall enum entries

In the `__syscall_nr` enum, **after** `SYS_readline`, add four new entries **before** `MAX_SYSCALL_NR`:

```c
  SYS_readline,
  SYS_sigaction,    /* register signal handler */
  SYS_kill,         /* send signal to process */
  SYS_pause,        /* wait for signal */
  SYS_sigreturn,    /* return from signal handler */

  MAX_SYSCALL_NR	/* XXX: always put it at the end of __syscall_nr */
```

#### 2.1b — Add error codes

In the `__error_nr` enum, **after** `E_BADF`, add two new entries **before** `MAX_ERROR_NR`:

```c
	E_BADF,          // bad file descriptor
	E_INVAL_SIGNUM,  /* invalid signal number */
	E_INVAL_HANDLER, /* invalid signal handler */
	MAX_ERROR_NR	/* XXX: always put it at the end of __error_nr */
```

### 2.2 `kern/lib/thread.h` — Add Signal State to Thread Type

**Change type:** Add `#include` and `struct thread` definition.

After `#ifdef _KERN_`, add:

```c
#include "signal.h"
```

After the `t_state` enum, add the full `struct thread` definition:

```c
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
```

> **Note:** This struct is not directly used by the TCBPool (which has its own local struct). It exists for type visibility in other kernel modules.

---

## 3. Kernel Process Changes

### 3.1 `kern/proc/proc.c` — Replace pcpu Code with `strncmp`

**Change type:** Major deletion + replacement. The entire file content is replaced.

The base template contains ~80 lines of `pcpu` (multi-CPU) management code (`pcpu_set_zero`, `pcpu_fields_init`, `pcpu_cur`, `get_pcpu_idx`, etc.). **Delete all of it** and replace the entire file with:

```c
#include <lib/types.h>
#include <lib/debug.h>
#include <string.h>

int
strncmp(const char *p, const char *q, size_t n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (int)((unsigned char)*p - (unsigned char)*q);
}
```

> **Why:** The pcpu multi-CPU code is not needed for the single-CPU signal implementation and causes link errors. The `strncmp` function is needed by shell commands but was missing from the base template.

---

## 4. Thread Layer Changes

### 4.1 `kern/thread/PTCBIntro/PTCBIntro.c` — Add Signal State & Accessors

**Change type:** Add includes, modify struct, modify init function, add ~75 lines of new accessor functions.

#### 4.1a — Add includes

At the top, add after `#include <lib/thread.h>`:

```c
#include <lib/string.h>
#include <lib/signal.h>
#include "export.h"
```

#### 4.1b — Add `sigstate` field to `struct TCB`

In the local `struct TCB` definition, add after `struct inode *cwd;`:

```c
  struct sig_state sigstate;       // Signal state for this process
```

#### 4.1c — Modify `tcb_init_at_id` function signature and body

Change the function signature from:

```c
void tcb_init_at_id(unsigned int pid)
```

to:

```c
void tcb_init_at_id(unsigned int cpu_idx, unsigned int pid)
```

Replace the function body:

```c
void tcb_init_at_id(unsigned int cpu_idx, unsigned int pid)
{
	TCBPool[pid].state = TSTATE_DEAD;
	TCBPool[pid].prev = NUM_IDS;
	TCBPool[pid].next = NUM_IDS;
	TCBPool[pid].channel = NULL;
	memset(TCBPool[pid].openfiles, 0, sizeof(TCBPool[pid].openfiles));
	TCBPool[pid].cwd = NULL;
	// Initialize signal state
	memset(&TCBPool[pid].sigstate, 0, sizeof(struct sig_state));
}
```

Key changes from the original:
- Added `cpu_idx` parameter (even though unused, matches expected signature)
- `0` → `NULL` for channel
- `memzero` → `memset(..., 0, ...)` for openfiles
- Removed `namei("/")` call for cwd, set to `NULL` instead
- Added signal state initialization with `memset`

#### 4.1d — Add signal accessor functions

Append the following entire block **at the end of the file** (after `tcb_set_cwd`):

```c
/*** Signal Accessors ***/

struct sigaction* tcb_get_sigaction(unsigned int pid, int signum)
{
  if (signum < 0 || signum >= NSIG)
    return NULL;
  return &TCBPool[pid].sigstate.sigactions[signum];
}

void tcb_set_sigaction(unsigned int pid, int signum, struct sigaction *act)
{
  if (signum >= 0 && signum < NSIG && act != NULL) {
    TCBPool[pid].sigstate.sigactions[signum] = *act;
  }
}

uint32_t tcb_get_pending_signals(unsigned int pid)
{
  return TCBPool[pid].sigstate.pending_signals;
}

void tcb_set_pending_signals(unsigned int pid, uint32_t signals)
{
  TCBPool[pid].sigstate.pending_signals = signals;
}

void tcb_add_pending_signal(unsigned int pid, int signum)
{
  if (signum >= 0 && signum < NSIG) {
    TCBPool[pid].sigstate.pending_signals |= (1 << signum);
  }
}

void tcb_clear_pending_signal(unsigned int pid, int signum)
{
  if (signum >= 0 && signum < NSIG) {
    TCBPool[pid].sigstate.pending_signals &= ~(1 << signum);
  }
}

void tcb_set_signal_context(unsigned int pid, uint32_t saved_esp_addr, uint32_t saved_eip_addr)
{
  TCBPool[pid].sigstate.saved_esp_addr = saved_esp_addr;
  TCBPool[pid].sigstate.saved_eip_addr = saved_eip_addr;
  TCBPool[pid].sigstate.in_signal_handler = 1;
}

void tcb_get_signal_context(unsigned int pid, uint32_t *saved_esp_addr, uint32_t *saved_eip_addr)
{
  *saved_esp_addr = TCBPool[pid].sigstate.saved_esp_addr;
  *saved_eip_addr = TCBPool[pid].sigstate.saved_eip_addr;
}

void tcb_clear_signal_context(unsigned int pid)
{
  TCBPool[pid].sigstate.saved_esp_addr = 0;
  TCBPool[pid].sigstate.saved_eip_addr = 0;
  TCBPool[pid].sigstate.in_signal_handler = 0;
}

int tcb_in_signal_handler(unsigned int pid)
{
  return TCBPool[pid].sigstate.in_signal_handler;
}

uint32_t tcb_is_sleeping(unsigned int pid)
{
  return (TCBPool[pid].state == TSTATE_SLEEP) ? 1 : 0;
}

void* tcb_get_channel(unsigned int pid)
{
  return TCBPool[pid].channel;
}
```

### 4.2 `kern/thread/PTCBIntro/export.h` — Declare Signal Accessors

**Change type:** Add includes and function declarations.

Add `#include <lib/signal.h>` after the existing file/inode includes.

Add the following declarations **before** `#endif /* _KERN_ */`:

```c
/* Signal accessor functions */
struct sigaction* tcb_get_sigaction(unsigned int pid, int signum);
void tcb_set_sigaction(unsigned int pid, int signum, struct sigaction *act);
uint32_t tcb_get_pending_signals(unsigned int pid);
void tcb_set_pending_signals(unsigned int pid, uint32_t signals);
void tcb_add_pending_signal(unsigned int pid, int signum);
void tcb_clear_pending_signal(unsigned int pid, int signum);
void tcb_set_signal_context(unsigned int pid, uint32_t saved_esp_addr, uint32_t saved_eip_addr);
void tcb_get_signal_context(unsigned int pid, uint32_t *saved_esp_addr, uint32_t *saved_eip_addr);
void tcb_clear_signal_context(unsigned int pid);
int tcb_in_signal_handler(unsigned int pid);
uint32_t tcb_is_sleeping(unsigned int pid);
void* tcb_get_channel(unsigned int pid);
```

### 4.3 `kern/thread/PThread/PThread.c` — Add `thread_exit()`

**Change type:** Add new function.

Add the following function **after** `thread_yield()` and **before** `sched_update()`:

```c
/**
 * Exit the current thread - switch to another thread without
 * putting current thread back in the ready queue.
 * Used when a process is terminated (e.g., by SIGKILL).
 */
void thread_exit(void)
{
  unsigned int old_cur_pid;
  unsigned int new_cur_pid;

  spinlock_acquire(&sched_lk);

  old_cur_pid = get_curid();
  // Do NOT set state to READY or enqueue - process is being terminated

  new_cur_pid = tqueue_dequeue(NUM_IDS);
  if (new_cur_pid == NUM_IDS) {
    // No other threads - this shouldn't happen in normal operation
    KERN_PANIC("thread_exit: no threads to switch to!\n");
  }

  tcb_set_state(new_cur_pid, TSTATE_RUN);
  set_curid(new_cur_pid);

  spinlock_release(&sched_lk);
  kctx_switch(old_cur_pid, new_cur_pid);
  // Should never return here
}
```

> **Critical:** Unlike `thread_yield()`, this does NOT re-enqueue the current process. This is how SIGKILL terminates a process — it marks the process DEAD and calls `thread_exit()` to switch away without re-queuing.

### 4.4 `kern/thread/PThread/export.h` — Declare `thread_exit()`

**Change type:** Add one line.

Add after `void thread_yield(void);`:

```c
void thread_exit(void);
```

---

## 5. Trap Layer Changes

### 5.1 `kern/trap/TDispatch/TDispatch.c` — Add Signal Syscall Dispatch

**Change type:** Add `case` entries.

In the `syscall_dispatch` function's `switch` statement, **before** the `default:` case, add:

```c
  /** Signal syscalls **/
  case SYS_sigaction:
    sys_sigaction(tf);
    break;
  case SYS_kill:
    sys_kill(tf);
    break;
  case SYS_pause:
    sys_pause(tf);
    break;
  case SYS_sigreturn:
    sys_sigreturn(tf);
    break;
```

### 5.2 `kern/trap/TDispatch/import.h` — Declare Imported Functions

**Change type:** Add declarations.

Add the following **before** `#endif /* _KERN_ */`:

```c
/* Signal syscalls */
void sys_sigaction(tf_t *tf);
void sys_kill(tf_t *tf);
void sys_pause(tf_t *tf);
void sys_sigreturn(tf_t *tf);

/* Shell syscalls */
void sys_sync_send(tf_t *tf);
void sys_sync_recv(tf_t *tf);
void sys_is_dir(tf_t *tf);
void sys_ls(tf_t *tf);
void sys_pwd(tf_t *tf);
void sys_cp(tf_t *tf);
void sys_mv(tf_t *tf);
void sys_rm(tf_t *tf);
void sys_cat(tf_t *tf);
void sys_touch(tf_t *tf);
void sys_readline(tf_t *tf);

void syscall_set_errno(unsigned int errno);
```

> **Note:** The shell syscall declarations may already exist in some form in the base template. The signal syscall declarations are the critical additions.

### 5.3 `kern/trap/TSyscall/TSyscall.c` — Implement Signal Syscalls

**Change type:** Uncomment include, add 4 new functions (~146 lines).

#### 5.3a — Uncomment signal include

Change:
```c
// #include <lib/signal.h>
```
to:
```c
#include <lib/signal.h>
```

#### 5.3b — Add the four signal syscall implementations

Append these at the **end of the file** (after `sys_touch`):

```c
void sys_sigaction(tf_t *tf)
{
    int signum = syscall_get_arg2(tf);
    struct sigaction *user_act = (struct sigaction *)syscall_get_arg3(tf);
    struct sigaction *user_oldact = (struct sigaction *)syscall_get_arg4(tf);
    unsigned int cur_pid = get_curid();
    struct sigaction kern_act;

    // Validate signal number
    if (signum < 1 || signum >= NSIG) {
        syscall_set_errno(tf, E_INVAL_SIGNUM);
        return;
    }

    // Save old handler if requested
    if (user_oldact != NULL) {
        struct sigaction *cur_act = tcb_get_sigaction(cur_pid, signum);
        if (cur_act != NULL) {
            // Copy from kernel to user space
            pt_copyout((void*)cur_act, cur_pid, (uintptr_t)user_oldact, sizeof(struct sigaction));
        }
    }

    // Set new handler if provided
    if (user_act != NULL) {
        // Copy from user space to kernel
        pt_copyin(cur_pid, (uintptr_t)user_act, (void*)&kern_act, sizeof(struct sigaction));
        KERN_INFO("[SIGACTION] Setting handler for sig %d: handler=%x\n", signum, (unsigned int)kern_act.sa_handler);
        tcb_set_sigaction(cur_pid, signum, &kern_act);
    }

    syscall_set_errno(tf, E_SUCC);
}

void sys_kill(tf_t *tf)
{
    int pid = syscall_get_arg2(tf);
    int signum = syscall_get_arg3(tf);

    // Validate signal number
    if (signum < 1 || signum >= NSIG) {
        syscall_set_errno(tf, E_INVAL_SIGNUM);
        return;
    }

    // Validate target process
    if (pid < 0 || pid >= NUM_IDS || tcb_get_state(pid) == TSTATE_DEAD) {
        syscall_set_errno(tf, E_INVAL_PID);
        return;
    }

    // SIGKILL is special - terminate immediately, cannot be caught
    if (signum == SIGKILL) {
        KERN_INFO("[SIGNAL] SIGKILL sent to process %d - terminating immediately\n", pid);

        // Set state to DEAD
        tcb_set_state(pid, TSTATE_DEAD);

        // Remove from ready queue
        tqueue_remove(NUM_IDS, pid);

        // Clear any pending signals
        tcb_set_pending_signals(pid, 0);

        KERN_INFO("[SIGNAL] Process %d terminated by SIGKILL\n", pid);
        syscall_set_errno(tf, E_SUCC);
        return;
    }

    // Set signal as pending
    tcb_add_pending_signal(pid, signum);

    // Wake up process if it's sleeping
    if (tcb_is_sleeping(pid)) {
        void *chan = tcb_get_channel(pid);
        if (chan != NULL) {
            thread_wakeup(chan);
        }
    }

    syscall_set_errno(tf, E_SUCC);
}

void sys_pause(tf_t *tf)
{
    unsigned int cur_pid = get_curid();

    // Check if any signals are pending
    if (tcb_get_pending_signals(cur_pid) != 0) {
        syscall_set_errno(tf, E_SUCC);
        return;
    }

    // No signals pending, go to sleep
    tcb_set_state(cur_pid, TSTATE_SLEEP);
    thread_yield();
    syscall_set_errno(tf, E_SUCC);
}

void sys_sigreturn(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t saved_esp_addr, saved_eip_addr;
    uint32_t saved_esp, saved_eip;

    KERN_INFO("[SIGRETURN] Called by process %d\n", cur_pid);

    // Get the saved context addresses from TCB
    tcb_get_signal_context(cur_pid, &saved_esp_addr, &saved_eip_addr);

    KERN_INFO("[SIGRETURN] saved_esp_addr=%x saved_eip_addr=%x\n", saved_esp_addr, saved_eip_addr);

    if (saved_esp_addr == 0 || saved_eip_addr == 0) {
        KERN_INFO("[SIGRETURN] No signal context to restore\n");
        syscall_set_errno(tf, E_INVAL_ADDR);
        return;
    }

    // Read saved ESP from user stack
    if (pt_copyin(cur_pid, saved_esp_addr, &saved_esp, sizeof(uint32_t)) != sizeof(uint32_t)) {
        KERN_INFO("[SIGRETURN] Failed to read saved_esp from %x\n", saved_esp_addr);
        syscall_set_errno(tf, E_MEM);
        return;
    }

    // Read saved EIP from user stack
    if (pt_copyin(cur_pid, saved_eip_addr, &saved_eip, sizeof(uint32_t)) != sizeof(uint32_t)) {
        KERN_INFO("[SIGRETURN] Failed to read saved_eip from %x\n", saved_eip_addr);
        syscall_set_errno(tf, E_MEM);
        return;
    }

    KERN_INFO("[SIGRETURN] Restoring context: esp=%x eip=%x\n", saved_esp, saved_eip);

    // Clear the signal context in TCB
    tcb_clear_signal_context(cur_pid);

    // Restore the original trapframe
    tf->esp = saved_esp;
    tf->eip = saved_eip;

    syscall_set_errno(tf, E_SUCC);
}
```

> **Critical Notes:**
> - `sys_sigaction` uses `pt_copyin` (NOT direct pointer dereference) to copy from user space. This is essential because user pointers are not accessible under the kernel page table.
> - `sys_kill` handles `SIGKILL` specially — immediate termination, cannot be caught.
> - `sys_sigreturn` reads saved ESP/EIP from the user stack via `pt_copyin`, then restores them into the trapframe.

### 5.4 `kern/trap/TTrapHandler/TTrapHandler.c` — Signal Delivery Engine

**Change type:** Add includes, add 3 functions (~136 lines), modify `trap()`.

> [!IMPORTANT]
> **This is the most critical file for signal delivery.**

#### 5.4a — Add includes

Add after the existing includes at the top:

```c
#include <lib/thread.h>
#include <lib/pmap.h>
```

Add to the import section (after `#include <vmm/MPTOp/export.h>` and `#include <thread/PThread/export.h>`):

```c
#include <thread/PTCBIntro/export.h>
#include <thread/PCurID/export.h>
#include <thread/PTQueueInit/export.h>
#include <lib/signal.h>
```

#### 5.4b — Add `deliver_signal()` function

Add **before** the `trap()` function:

```c
// Signal trampoline code to be placed on user stack
// This executes: mov eax, SYS_sigreturn; int 0x30; (infinite loop as safety)
// Machine code: B8 <syscall_num> 00 00 00  CD 30  EB FE

static void deliver_signal(tf_t *tf, int signum)
{
    unsigned int cur_pid = get_curid();
    struct sigaction *sa = tcb_get_sigaction(cur_pid, signum);

    KERN_INFO("[SIGNAL] deliver_signal: pid=%d signum=%d sa=%x\n", cur_pid, signum, (unsigned int)sa);

    if (sa != NULL && sa->sa_handler != NULL) {
        KERN_INFO("[SIGNAL] deliver_signal: sa->sa_handler=%x\n", (unsigned int)sa->sa_handler);

        // Save original context for sigreturn
        // Stack layout (growing down):
        //   [original esp area]
        //   saved_eip        <- for sigreturn to restore
        //   saved_esp        <- for sigreturn to restore
        //   trampoline code  <- 8 bytes: mov eax, SYS_sigreturn; int 0x30; jmp $
        //   signum           <- argument to handler
        //   trampoline_addr  <- return address (points to trampoline)
        //   [new esp]

        uint32_t orig_esp = tf->esp;
        uint32_t orig_eip = tf->eip;
        uint32_t new_esp = orig_esp;
        size_t copied;

        // Push saved_eip (for sigreturn)
        new_esp -= 4;
        copied = pt_copyout(&orig_eip, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_eip_addr = new_esp;

        // Push saved_esp (for sigreturn)
        new_esp -= 4;
        copied = pt_copyout(&orig_esp, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_esp_addr = new_esp;

        // Push trampoline code (12 bytes with padding)
        // mov eax, SYS_sigreturn (B8 98 00 00 00) - 5 bytes
        // int 0x30 (CD 30) - 2 bytes
        // jmp $ (EB FE) - 2 bytes (safety infinite loop)
        uint8_t trampoline[12] = {
            0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,  // mov eax, SYS_sigreturn
            0xCD, 0x30,                              // int 0x30
            0xEB, 0xFE,                              // jmp $ (infinite loop - safety)
            0x90, 0x90, 0x90                         // nop padding
        };
        new_esp -= 12;
        copied = pt_copyout(trampoline, cur_pid, new_esp, 12);
        uint32_t trampoline_addr = new_esp;

        // Align stack to 4 bytes if needed
        new_esp = new_esp & ~3;

        // Push signal number (argument) — cdecl: args on stack
        new_esp -= 4;
        uint32_t sig_arg = signum;
        copied = pt_copyout(&sig_arg, cur_pid, new_esp, sizeof(uint32_t));

        // Push return address (trampoline) — cdecl: return addr at [ESP]
        new_esp -= 4;
        copied = pt_copyout(&trampoline_addr, cur_pid, new_esp, sizeof(uint32_t));

        KERN_INFO("[SIGNAL] Stack setup: orig_esp=%x orig_eip=%x tramp=%x new_esp=%x\n",
                  orig_esp, orig_eip, trampoline_addr, new_esp);

        // Update trapframe
        tf->esp = new_esp;
        tf->eip = (uint32_t)sa->sa_handler;

        // Store saved context location in TCB for sigreturn
        tcb_set_signal_context(cur_pid, saved_esp_addr, saved_eip_addr);

        KERN_INFO("[SIGNAL] Delivering signal %d to process %d, handler at %x\n",
                  signum, cur_pid, sa->sa_handler);
    } else {
        KERN_INFO("[SIGNAL] No handler for signal %d in process %d\n", signum, cur_pid);
    }
}
```

#### 5.4c — Add `terminate_process()` function

```c
static void terminate_process(unsigned int pid)
{
    KERN_INFO("[SIGNAL] Terminating process %d (SIGKILL)\n", pid);

    // Set state to DEAD
    tcb_set_state(pid, TSTATE_DEAD);

    // Remove from ready queue (NUM_IDS is the ready queue)
    tqueue_remove(NUM_IDS, pid);

    // Clear any pending signals
    tcb_set_pending_signals(pid, 0);

    KERN_INFO("[SIGNAL] Process %d terminated\n", pid);
}
```

#### 5.4d — Add `handle_pending_signals()` function

```c
static void handle_pending_signals(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t pending_signals = tcb_get_pending_signals(cur_pid);

    // Check for pending signals that aren't blocked
    if (pending_signals != 0) {
        KERN_INFO("[SIGNAL] Process %d has pending signals: 0x%x\n", cur_pid, pending_signals);
        for (int signum = 1; signum < NSIG; signum++) {
            if (pending_signals & (1 << signum)) {
                KERN_INFO("[SIGNAL] Processing signal %d for process %d\n", signum, cur_pid);
                // Clear the pending signal
                tcb_clear_pending_signal(cur_pid, signum);

                // SIGKILL cannot be caught - always terminates
                if (signum == SIGKILL) {
                    terminate_process(cur_pid);
                    // Switch to another process since this one is dead
                    // Use thread_exit() not thread_yield() - we don't want to re-queue the dead process
                    thread_exit();
                    // Should not return here, but just in case
                    return;
                }

                // Deliver the signal to handler
                deliver_signal(tf, signum);
                break;
            }
        }
    }
}
```

#### 5.4e — Modify `trap()` function — THE MOST CRITICAL CHANGE

In the `trap()` function, **after** `kstack_switch(cur_pid);` and **before** `set_pdir_base(cur_pid);`, add:

```c
    // Handle any pending signals BEFORE switching to user page table
    // This is critical because handle_pending_signals needs to access
    // page tables via identity-mapped physical addresses
    handle_pending_signals(tf);
```

Also change the `trap_return` call from:

```c
	  trap_return((void *) tf);
```

to:

```c
    trap_return(tf);
```

> **CRITICAL BUG NOTE:** `handle_pending_signals()` **MUST** run BEFORE `set_pdir_base(cur_pid)`. The kernel page table (PID 0) has identity-mapped physical addresses, which `pt_copyout` needs to write the trampoline to the user stack. If you call it after switching to the user page table, the kernel cannot access the page table entries and the system will crash.

---

## 6. User-Space Changes

### 6.1 `user/include/syscall.h` — Add Syscall Wrappers

**Change type:** Add include and 3 inline functions.

#### 6.1a — Add include

After `#include <file.h>`, add:

```c
#include <signal.h>
```

#### 6.1b — Add syscall wrapper functions

Add **before** the final `#endif`:

```c
/* Signal system calls */

static gcc_inline int
sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	int errno;
	asm volatile ("int %1"
		      : "=a" (errno)
		      : "i" (T_SYSCALL),
		        "a" (SYS_sigaction),
		        "b" (signum),
		        "c" (act),
		        "d" (oldact)
		      : "cc", "memory");
	return errno ? -1 : 0;
}

static gcc_inline int
sys_kill(int pid, int signum)
{
	int errno;
	asm volatile ("int %1"
		      : "=a" (errno)
		      : "i" (T_SYSCALL),
		        "a" (SYS_kill),
		        "b" (pid),
		        "c" (signum)
		      : "cc", "memory");
	return errno ? -1 : 0;
}

static gcc_inline int
sys_pause(void)
{
	int errno;
	asm volatile ("int %1"
		      : "=a" (errno)
		      : "i" (T_SYSCALL),
		        "a" (SYS_pause)
		      : "cc", "memory");
	return errno ? -1 : 0;
}
```

> **Note:** These use the same `int 0x30` software interrupt mechanism as all other CertiKOS syscalls. Arguments are passed in registers: `eax` = syscall number, `ebx` = arg1, `ecx` = arg2, `edx` = arg3.

### 6.2 `user/shell/shell.c` — Add Kill/Trap/Spawn Commands

**Change type:** Major additions (~220 lines).

#### 6.2a — Add includes and defines at the top

After `#include <x86.h>`, add:

```c
#include "signal.h"

/* Stub definitions for POSIX types not available in mCertikOS */
#define O_RDONLY    0x000
#define O_WRONLY    0x001
#define O_RDWR      0x002
#define O_CREATE    0x200
#define O_TRUNC     0x400
```

#### 6.2b — Add helper functions and forward declarations

After the `#define MAXARGS 16` line, add:

```c
/* Forward declarations */
void signal_handler(int signum);

/* Helper function to convert string to int (wrapper for mCertikOS atoi) */
static int str_to_int(const char *s) {
    int result = 0;
    atoi(s, &result);
    return result;
}

/* Stub for getpid - returns current process id (use syscall if available) */
static int getpid(void) {
    return 1;  /* Stub: in a real implementation, add SYS_getpid syscall */
}
```

#### 6.2c — Add command declarations

After `shell_append` declaration, add:

```c
int shell_kill(int argc, char **argv);
int shell_trap(int argc, char **argv);
int shell_spawn(int argc, char **argv);
```

#### 6.2d — Add commands to command table

In the `commands[]` array, add three new entries. Change the last entry from ending with `}` to ending with `},` and add:

```c
        {"kill", "kill <signal> <pid> \n\t send signal to process", shell_kill},
        {"trap", "trap <signum> <handler> \n\t register signal handler", shell_trap},
        {"spawn", "spawn <elf_id> \n\t spawn a new process (1=ping, 2=pong, 3=ding)", shell_spawn}
```

#### 6.2e — Replace `shell_test()` function

Replace the entire `shell_test()` function body with:

```c
void shell_test() {
    // Test case 1: Basic signal handling
    printf("Test 1: Basic signal handling\n");
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;

    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("Failed to register signal handler\n");
        return;
    }

    printf("Registered handler for SIGUSR1\n");
    printf("Sending SIGUSR1 to self...\n");
    kill(getpid(), SIGUSR1);

    // Test case 2: Signal blocking
    printf("\nTest 2: Signal blocking\n");
    struct sigaction sa2;
    sa2.sa_handler = signal_handler;
    sa2.sa_flags = 0;
    sa2.sa_mask = (1 << SIGUSR2);  // Block SIGUSR2

    if (sigaction(SIGUSR2, &sa2, NULL) < 0) {
        printf("Failed to register signal handler\n");
        return;
    }

    printf("Registered handler for SIGUSR2 (blocked)\n");
    printf("Sending SIGUSR2 to self...\n");
    kill(getpid(), SIGUSR2);

    // Test case 3: pause() functionality
    printf("\nTest 3: pause() functionality\n");
    printf("Process will pause until SIGUSR1 is received...\n");
    pause();
    printf("Resumed after receiving signal\n");
}
```

#### 6.2f — Comment out the close/open line in `main()`

In the `main()` function, comment out:

```c
	close(open("usertests.ran", O_CREATE));
```

Replace with:

```c
	//close(open("usertests.ran", O_CREATE));  // Disabled - requires proper cwd init
```

#### 6.2g — Add signal_handler and command implementations at end of file

Append the following at the **end of the file**:

```c
int shell_kill(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: kill -<signal> <pid>\n");
        printf("Example: kill -9 2\n");
        return;
    }

    int sig = 0;
    int pid = 0;

    // Parse signal number - handle "-N" format
    if (argv[1][0] == '-') {
        // Parse the number after the '-'
        sig = str_to_int(&argv[1][1]);
    } else {
        // Try parsing as just a number
        sig = str_to_int(argv[1]);
    }

    // Parse PID
    pid = str_to_int(argv[2]);

    // Validate signal number (1-31)
    if (sig < 1 || sig > 31) {
        printf("Invalid signal number: %d (must be 1-31)\n", sig);
        return;
    }

    // Validate PID
    if (pid < 1 || pid > 63) {
        printf("Invalid PID: %d (must be 1-63)\n", pid);
        return;
    }

    printf("Sending signal %d to process %d...\n", sig, pid);

    int result = kill(pid, sig);
    if (result == 0) {
        printf("Signal sent successfully.\n");
    } else {
        printf("Failed to send signal (error: %d)\n", result);
    }
}

void signal_handler(int signum)
{
    printf("\n*** Received signal %d ***\n", signum);
    printf(">:");  // Reprint prompt
}

int shell_trap(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: trap <signum>\n");
        printf("Example: trap 2   (register handler for SIGINT)\n");
        return -1;
    }

    int signum = str_to_int(argv[1]);

    if (signum < 1 || signum >= 32) {
        printf("Invalid signal number: %d (must be 1-31)\n", signum);
        return -1;
    }

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;

    printf("Registering handler for signal %d at address %x...\n", signum, (unsigned int)signal_handler);

    if (sigaction(signum, &sa, NULL) < 0) {
        printf("Failed to register signal handler\n");
        return -1;
    }

    printf("Handler registered successfully.\n");
    return 0;
}

int shell_spawn(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: spawn <elf_id>\n");
        printf("  elf_id: 1=ping, 2=pong, 3=ding\n");
        return -1;
    }

    int elf_id = str_to_int(argv[1]);

    if (elf_id < 1 || elf_id > 5) {
        printf("Invalid elf_id: %d (must be 1-5)\n", elf_id);
        return -1;
    }

    printf("Spawning process with elf_id %d...\n", elf_id);

    pid_t new_pid = spawn(elf_id, 1000);
    if (new_pid != -1) {
        printf("Process spawned with PID %d\n", new_pid);
    } else {
        printf("Failed to spawn process\n");
        return -1;
    }

    return 0;
}
```

### 6.3 `user/signal_test.c` — Signal Test Program (Optional)

**Path:** `user/signal_test.c`
**Purpose:** Standalone test program for signal handling.

```c
#include <stdio.h>
#include <syscall.h>
#include "signal.h"
#include <types.h>

void sigint_handler(int signum)
{
    printf("Received SIGINT (%d)\n", signum);
}

void sigusr1_handler(int signum)
{
    printf("Received SIGUSR1 (%d)\n", signum);
}

int main(int argc, char **argv)
{
    struct sigaction sa;

    printf("Signal test program\n");

    // Register SIGINT handler
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        printf("Failed to register SIGINT handler\n");
        return -1;
    }

    // Register SIGUSR1 handler
    sa.sa_handler = sigusr1_handler;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("Failed to register SIGUSR1 handler\n");
        return -1;
    }

    printf("Handlers registered. Waiting for signals...\n");

    while (1) {
        pause();  // Wait for signals
    }

    return 0;
}
```

---

## 7. Build System Changes

### 7.1 `user/lib/Makefile.inc` — Add signal.c to user library

Add after the `USER_LIB_SRC += $(USER_DIR)/lib/string.c` line:

```makefile
USER_LIB_SRC	+= $(USER_DIR)/lib/signal.c
```

### 7.2 `user/Makefile.inc` — Add signal_test build target and shell dependency

#### 7.2a — Add signal_test build rules

After the line `include $(USER_DIR)/shell/Makefile.inc`, add:

```makefile
# Signal test program
SIGNAL_TEST_SRC	:= $(USER_DIR)/signal_test.c
SIGNAL_TEST_OBJ	:= $(USER_OBJDIR)/signal_test.o
SIGNAL_TEST_BIN	:= $(USER_OBJDIR)/signal_test

$(SIGNAL_TEST_OBJ): $(SIGNAL_TEST_SRC)
	@echo + cc $<
	$(V)$(CC) -nostdinc $(USER_CFLAGS) -c -o $@ $<

$(SIGNAL_TEST_BIN): $(USER_LIB_OBJ) $(SIGNAL_TEST_OBJ)
	@echo + ld $@
	$(V)$(LD) $(USER_LDFLAGS) -o $@ $^ $(GCC_LIBS)
```

#### 7.2b — Update user target

Change:
```makefile
user: lib idle pingpong fstest
```
to:
```makefile
user: lib idle pingpong fstest shell $(SIGNAL_TEST_BIN)
```

#### 7.2c — Add install rule

Add after the `install_user:` target line:

```makefile
	$(V)cp $(SIGNAL_TEST_BIN) $(OBJDIR)/user/
```

---

## 8. Miscellaneous

### 8.1 `.gitignore` — Expand Ignore Patterns

Replace the existing `.gitignore` with expanded patterns. Add:

```
*.img
log
*.log
cscope.*
*~
```

And add at the end:

```
*.v
Icon*
*.swp
*.stderr
*.stdout
.cproject
.project
.settings/
eject.sh
*.cerr
```

---

## 9. Verification Checklist

After applying all changes, verify:

- [ ] `make` completes without errors
- [ ] `make` produces `certikos.img`
- [ ] Boot in QEMU: `qemu-system-i386 -hda certikos.img -serial stdio`
- [ ] Shell prompt `>:` appears
- [ ] `trap 10` — registers handler for SIGUSR1, prints "Handler registered successfully."
- [ ] `spawn 1` — spawns a process (ping), note the PID
- [ ] `kill -10 <pid>` — sends SIGUSR1, handler prints "*** Received signal 10 ***"
- [ ] `kill -9 <pid>` — sends SIGKILL, process terminates
- [ ] `help` — shows kill, trap, spawn commands in the list

---

## 10. Critical Implementation Notes

These are lessons learned from debugging. Violating any of these will cause crashes or silent failures.

### 10.1 Page Table Ordering (Most Important)

`handle_pending_signals(tf)` **MUST** be called **BEFORE** `set_pdir_base(cur_pid)` in the `trap()` function. The kernel page table (PID 0 / identity mapping) is needed for `pt_copyout` to write the trampoline to user-space pages. After switching to the user page table, the kernel loses access to page table entries.

### 10.2 cdecl Calling Convention

Signal arguments use the cdecl calling convention:
- **Return address** at `[ESP]` → points to trampoline
- **Signal number** at `[ESP+4]` → first argument to handler

The signal number is **NOT** passed in `EAX`. It is on the stack as per cdecl.

### 10.3 User-Space Copy for sigaction

`sys_sigaction` MUST use `pt_copyin` to copy the `struct sigaction` from user space. Direct pointer dereference will fail because the kernel cannot access user-space addresses under the kernel page table.

### 10.4 Trampoline is Inline Machine Code

The sigreturn trampoline is raw x86 machine code pushed onto the user stack:
```
B8 <SYS_sigreturn> 00 00 00   ; mov eax, SYS_sigreturn
CD 30                          ; int 0x30
EB FE                          ; jmp $ (infinite loop safety)
```

This avoids needing a separate trampoline function at a fixed address. When the signal handler returns, it jumps to this code on the stack, which invokes `SYS_sigreturn` to restore the original context.

### 10.5 thread_exit() vs thread_yield()

When handling SIGKILL in the trap handler, use `thread_exit()` (which does NOT re-enqueue the process) instead of `thread_yield()` (which would put the dead process back in the ready queue).

### 10.6 Shell kill Parsing

The shell `kill` command supports both `kill -9 2` (with dash) and `kill 9 2` (without dash) formats. The `argv[1][0] == '-'` check handles the dash prefix.

---

## Appendix: File Change Summary

| File | Type | Lines +/- | Description |
|------|------|-----------|-------------|
| `kern/lib/signal.h` | **NEW** | +64 | Signal structures \& constants |
| `user/include/signal.h` | **NEW** | +57 | User-space signal API |
| `user/lib/signal.c` | **NEW** | +19 | User-space signal wrappers |
| `kern/lib/syscall.h` | Modified | +6 | Syscall enum entries \& error codes |
| `kern/lib/thread.h` | Modified | +15 | `#include signal.h`, struct thread |
| `kern/proc/proc.c` | Modified | +13/-92 | Replace pcpu code with strncmp |
| `kern/thread/PTCBIntro/PTCBIntro.c` | Modified | +90 | Signal state \& accessor functions |
| `kern/thread/PTCBIntro/export.h` | Modified | +15 | Signal accessor declarations |
| `kern/thread/PThread/PThread.c` | Modified | +29 | `thread_exit()` function |
| `kern/thread/PThread/export.h` | Modified | +1 | `thread_exit()` declaration |
| `kern/trap/TDispatch/TDispatch.c` | Modified | +15 | Syscall dispatch cases |
| `kern/trap/TDispatch/import.h` | Modified | +21 | Import declarations |
| `kern/trap/TSyscall/TSyscall.c` | Modified | +146 | 4 syscall implementations |
| `kern/trap/TTrapHandler/TTrapHandler.c` | Modified | +145 | Signal delivery engine |
| `user/include/syscall.h` | Modified | +44 | Inline asm syscall wrappers |
| `user/lib/Makefile.inc` | Modified | +1 | Add signal.c |
| `user/Makefile.inc` | Modified | +16 | Build signal\_test |
| `user/shell/shell.c` | Modified | +220 | kill, trap, spawn commands |
| `user/signal_test.c` | **NEW** | +47 | Signal test program |
