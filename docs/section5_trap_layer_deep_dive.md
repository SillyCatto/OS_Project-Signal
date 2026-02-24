# Section 5 Deep Dive: Trap Layer Changes

## The Heart of Signal Handling in mCertiKOS

> This document is a companion to Section 5 of `changes_guide.md`. It explains **every piece** of the trap layer changes required for signal handling — the syscall implementations, the signal delivery engine, the raw assembly trampoline, and the trap return path. If you read this document front-to-back, you will understand exactly how a signal enters the kernel, how the kernel manipulates the user stack and trap frame to redirect execution, and how the user process eventually returns to its original code as if nothing happened.

---

## Table of Contents

- [Section 5 Deep Dive: Trap Layer Changes](#section-5-deep-dive-trap-layer-changes)
  - [The Heart of Signal Handling in mCertiKOS](#the-heart-of-signal-handling-in-mcertikos)
  - [Table of Contents](#table-of-contents)
  - [1. The Big Picture: Where Section 5 Fits](#1-the-big-picture-where-section-5-fits)
  - [2. Prerequisite Concepts](#2-prerequisite-concepts)
    - [2.1 The Trap Frame (`tf_t`)](#21-the-trap-frame-tf_t)
    - [2.2 Syscall Calling Convention](#22-syscall-calling-convention)
    - [2.3 `pt_copyin` and `pt_copyout` — Crossing Address Spaces](#23-pt_copyin-and-pt_copyout--crossing-address-spaces)
    - [2.4 Bitmask Operations for Pending Signals](#24-bitmask-operations-for-pending-signals)
  - [3. Subsection 5.1 — TDispatch.c: The Syscall Routing Table](#3-subsection-51--tdispatchc-the-syscall-routing-table)
  - [4. Subsection 5.2 — TDispatch/import.h: Function Declarations](#4-subsection-52--tdispatchimporth-function-declarations)
  - [5. Subsection 5.3 — TSyscall.c: The Four Signal Syscalls](#5-subsection-53--tsyscallc-the-four-signal-syscalls)
    - [5.1 `sys_sigaction` — Registering a Signal Handler](#51-sys_sigaction--registering-a-signal-handler)
    - [5.2 `sys_kill` — Sending a Signal to a Process](#52-sys_kill--sending-a-signal-to-a-process)
    - [5.3 `sys_pause` — Waiting for a Signal](#53-sys_pause--waiting-for-a-signal)
    - [5.4 `sys_sigreturn` — Returning from a Signal Handler](#54-sys_sigreturn--returning-from-a-signal-handler)
  - [6. Subsection 5.4 — TTrapHandler.c: The Signal Delivery Engine](#6-subsection-54--ttraphandlerc-the-signal-delivery-engine)
    - [6.1 `deliver_signal` — Constructing the Signal Stack Frame](#61-deliver_signal--constructing-the-signal-stack-frame)
    - [6.2 The Trampoline: 12 Bytes That Make Everything Work](#62-the-trampoline-12-bytes-that-make-everything-work)
    - [6.3 `terminate_process` — The SIGKILL Path](#63-terminate_process--the-sigkill-path)
    - [6.4 `handle_pending_signals` — The Signal Check Loop](#64-handle_pending_signals--the-signal-check-loop)
    - [6.5 The Modified `trap()` Function — The Critical Ordering](#65-the-modified-trap-function--the-critical-ordering)
  - [7. The `trap_return` Assembly — How the CPU Resumes User Code](#7-the-trap_return-assembly--how-the-cpu-resumes-user-code)
  - [8. End-to-End Walkthrough: Signal Lifecycle](#8-end-to-end-walkthrough-signal-lifecycle)
  - [9. Common Pitfalls and Lessons Learned](#9-common-pitfalls-and-lessons-learned)
    - [Pitfall 1: Direct User Pointer Access vs. `pt_copyin`](#pitfall-1-direct-user-pointer-access-vs-pt_copyin)
    - [Pitfall 2: Page Table Order in `trap()`](#pitfall-2-page-table-order-in-trap)
    - [Pitfall 3: Passing Signal Number in Register Instead of Stack](#pitfall-3-passing-signal-number-in-register-instead-of-stack)
    - [Pitfall 4: `thread_yield` vs. `thread_exit` for SIGKILL](#pitfall-4-thread_yield-vs-thread_exit-for-sigkill)
    - [Pitfall 5: Trampoline Syscall Number Mismatch](#pitfall-5-trampoline-syscall-number-mismatch)
    - [Pitfall 6: Missing Handler NULL Check](#pitfall-6-missing-handler-null-check)
  - [Summary](#summary)

---

## 1. The Big Picture: Where Section 5 Fits

The trap layer is the **execution boundary** between user space (Ring 3) and kernel space (Ring 0). Every syscall, every interrupt, and every exception passes through this layer. For signal handling, the trap layer is where:

1. **Signal syscalls arrive** — user code calls `int 0x30`, the CPU switches to Ring 0, and the kernel dispatches to `sys_sigaction`, `sys_kill`, `sys_pause`, or `sys_sigreturn`.
2. **Signals get delivered** — on every return from kernel to user, the kernel checks for pending signals and re-engineers the user stack so execution jumps to the signal handler instead of the original code.
3. **Signal handlers return** — after the handler finishes, a trampoline on the user stack triggers `sys_sigreturn`, which restores the original execution context.

```
 FILES MODIFIED IN SECTION 5:
 ┌─────────────────────────────────────────────────────────────┐
 │  kern/trap/TDispatch/TDispatch.c  ← syscall routing table  │
 │  kern/trap/TDispatch/import.h     ← function declarations  │
 │  kern/trap/TSyscall/TSyscall.c    ← syscall implementations│
 │  kern/trap/TTrapHandler/TTrapHandler.c ← delivery engine   │
 └─────────────────────────────────────────────────────────────┘
```

These four files form a pipeline:

```
 User code → int 0x30 → trap() → TDispatch → TSyscall → [back to trap()] → handle_pending_signals → deliver_signal → trap_return → user code
```

---

## 2. Prerequisite Concepts

Before diving into the code, you need to understand four things that come up repeatedly.

### 2.1 The Trap Frame (`tf_t`)

When the CPU transitions from Ring 3 to Ring 0 (via `int 0x30` or any trap), it saves the user's state into a **trap frame** — a struct on the kernel stack. This struct is the kernel's snapshot of exactly where the user was and what it was doing.

```c
// From kern/lib/trap.h
struct pushregs {        // pushed by 'pusha' instruction
    uint32_t edi;        // offset  0 from pushregs base
    uint32_t esi;        // offset  4
    uint32_t ebp;        // offset  8
    uint32_t oesp;       // offset 12  (useless — 'pusha' pushed value)
    uint32_t ebx;        // offset 16
    uint32_t edx;        // offset 20
    uint32_t ecx;        // offset 24
    uint32_t eax;        // offset 28
};

struct tf_t {
    pushregs regs;       // bytes  0-31: general-purpose registers
    uint16_t es;         // bytes 32-33: extra segment
    uint16_t padding_es; // bytes 34-35: padding
    uint16_t ds;         // bytes 36-37: data segment
    uint16_t padding_ds; // bytes 38-39: padding
    uint32_t trapno;     // bytes 40-43: trap number (48 for syscall)
    uint32_t err;        // bytes 44-47: error code
    uintptr_t eip;       // bytes 48-51: instruction pointer ← WHERE user was
    uint16_t cs;         // bytes 52-53: code segment
    uint16_t padding_cs; // bytes 54-55: padding
    uint32_t eflags;     // bytes 56-59: flags register
    uintptr_t esp;       // bytes 60-63: stack pointer ← user's stack top
    uint16_t ss;         // bytes 64-65: stack segment
    uint16_t padding_ss; // bytes 66-67: padding
};
```

**The two most important fields for signal handling:**

| Field | What it controls |
|-------|-----------------|
| `tf->eip` | Where the CPU will resume executing when returning to user mode. By changing this, the kernel redirects the user to a signal handler instead of where it was. |
| `tf->esp` | The user's stack pointer. By changing this, the kernel makes it look like data (the trampoline, signal number, etc.) was "pushed" onto the user stack. |

**Key insight:** The kernel can make the user process do *anything* after returning — just by modifying `tf->eip` and `tf->esp`. This is the fundamental mechanism of signal delivery. The user never knows it happened.

### 2.2 Syscall Calling Convention

CertiKOS uses a custom syscall convention where arguments are passed in registers. When user code executes `int 0x30`, the CPU traps into the kernel, and the trap entry code saves all registers into the trap frame. The kernel then reads arguments from the trap frame:

```
 Register → Getter Function    → Purpose
 ─────────────────────────────────────────────
 EAX      → syscall_get_arg1() → Syscall number (NOT a user argument!)
 EBX      → syscall_get_arg2() → First actual argument
 ECX      → syscall_get_arg3() → Second actual argument
 EDX      → syscall_get_arg4() → Third actual argument
 ESI      → syscall_get_arg5() → Fourth actual argument
 EDI      → syscall_get_arg6() → Fifth actual argument
```

**Why the off-by-one?** `syscall_get_arg1()` reads EAX, which holds the syscall number (e.g., `SYS_sigaction = 24`). The first *actual* argument to the syscall starts at `syscall_get_arg2()` (EBX). This naming is confusing but consistent throughout CertiKOS.

**Where this mapping is defined** (from `TSyscallArg.c`):
```c
unsigned int syscall_get_arg1(tf_t *tf) { return tf->regs.eax; }  // syscall number
unsigned int syscall_get_arg2(tf_t *tf) { return tf->regs.ebx; }  // arg 1
unsigned int syscall_get_arg3(tf_t *tf) { return tf->regs.ecx; }  // arg 2
unsigned int syscall_get_arg4(tf_t *tf) { return tf->regs.edx; }  // arg 3
```

**User-side example** — here's how `sys_sigaction` sets up registers before triggering the trap:
```c
// From user/include/syscall.h
static gcc_inline int
sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    int errno;
    asm volatile ("int %1"
          : "=a" (errno)              // output: EAX = errno after return
          : "i" (T_SYSCALL),          // interrupt number 48
            "a" (SYS_sigaction),      // EAX = syscall number
            "b" (signum),             // EBX = first argument  → syscall_get_arg2
            "c" (act),                // ECX = second argument → syscall_get_arg3
            "d" (oldact)              // EDX = third argument  → syscall_get_arg4
          : "cc", "memory");
    return errno ? -1 : 0;
}
```

Notice the constraint letters: `"a"` = EAX, `"b"` = EBX, `"c"` = ECX, `"d"` = EDX. The GCC inline assembly puts each argument into the exact register the kernel expects.

### 2.3 `pt_copyin` and `pt_copyout` — Crossing Address Spaces

In CertiKOS, each process has its own page table. The kernel cannot simply dereference a user pointer — that pointer is valid only in the user's address space. To read/write user memory, the kernel uses:

| Function | Direction | What it does |
|----------|-----------|-------------|
| `pt_copyin(pid, user_addr, kern_buf, size)` | User → Kernel | Reads `size` bytes from `user_addr` in process `pid`'s address space into `kern_buf` |
| `pt_copyout(kern_buf, pid, user_addr, size)` | Kernel → User | Writes `size` bytes from `kern_buf` to `user_addr` in process `pid`'s address space |

**How they work internally:** These functions walk the process's page table to translate the user virtual address to a physical address, then use the kernel's identity mapping (physical address = virtual address in PID 0's page table) to directly access the memory.

**Why this matters for signals:** The `struct sigaction` that the user passes to `sigaction()` lives in user memory. The kernel can't just cast the pointer and read it — it must use `pt_copyin` to safely copy it into a kernel-side buffer. Similarly, `deliver_signal` must use `pt_copyout` to write the trampoline code and arguments onto the user's stack.

```
 pt_copyin:   User Space ──────→ Kernel Space
              "bring data IN to the kernel"

 pt_copyout:  Kernel Space ────→ User Space
              "push data OUT to the user"
```

**Critical bug we hit:** An early version of `sys_sigaction` tried to directly dereference `user_act` (the user pointer) from the kernel. This caused a page fault because the kernel was running with PID 0's page table (the kernel page table), which doesn't map user addresses. The fix was to use `pt_copyin` to copy the `struct sigaction` from user space into a local kernel variable `kern_act`.

### 2.4 Bitmask Operations for Pending Signals

Pending signals are stored as a 32-bit bitmask in each process's TCB (Thread Control Block). Each bit position represents a signal number:

```
 Bit:    31 30 29 ... 15 14 ... 9  8  ...  2   1   0
 Signal: 31 30 29 ... 15 14 ... 9  8  ...  2   1  (unused)
         │              │         │
         │              SIGTERM   SIGKILL
         └── higher signals
```

- **Set a signal as pending:** `pending |= (1 << signum)` — turns on bit `signum`
- **Clear a pending signal:** `pending &= ~(1 << signum)` — turns off bit `signum`
- **Check if signal is pending:** `pending & (1 << signum)` — non-zero if set
- **Check if any signal is pending:** `pending != 0`

Signal 0 is not used (bit 0 is always 0). Valid signals range from 1 to NSIG-1 (1 to 31).

---

## 3. Subsection 5.1 — TDispatch.c: The Syscall Routing Table

`TDispatch.c` contains the `syscall_dispatch()` function — a giant switch statement that routes each syscall number to its implementation function. We add four new cases for signals:

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

**How this works:**

1. User executes `int 0x30` with `EAX = SYS_sigaction` (which is 24 in the enum)
2. CPU traps into kernel, saves state into `tf_t`
3. `trap()` (in TTrapHandler.c) calls the registered handler for trap 48
4. That handler is `syscall_dispatch()` in TDispatch.c
5. `syscall_dispatch()` reads `EAX` via `syscall_get_arg1(tf)` → gets 24
6. Switch hits `case SYS_sigaction:` → calls `sys_sigaction(tf)`
7. After the syscall function returns, control goes back to `trap()`, which eventually calls `trap_return` to resume the user

**Where do `SYS_sigaction`, `SYS_kill`, etc. come from?**

They're defined in `kern/lib/syscall.h` as an enum:
```c
enum __syscall_nr {
    SYS_puts = 0,
    SYS_spawn,
    SYS_yield,
    // ... many more ...
    SYS_sigaction,    // = 24  (register signal handler)
    SYS_kill,         // = 25  (send signal to process)
    SYS_pause,        // = 26  (wait for signal)
    SYS_sigreturn,    // = 27  (return from signal handler)
    MAX_SYSCALL_NR
};
```

The enum auto-increments, so `SYS_sigreturn` is 27. The exact numbers don't matter as long as both user and kernel headers agree (and the `SYS_sigreturn` constant in the trampoline matches).

---

## 4. Subsection 5.2 — TDispatch/import.h: Function Declarations

The import header tells the linker that `sys_sigaction`, `sys_kill`, `sys_pause`, and `sys_sigreturn` are implemented somewhere else (in TSyscall.c) and TDispatch.c needs to call them:

```c
/* Signal syscalls */
void sys_sigaction(tf_t *tf);
void sys_kill(tf_t *tf);
void sys_pause(tf_t *tf);
void sys_sigreturn(tf_t *tf);
```

All four follow the same function signature: they take a pointer to the trap frame (`tf_t *tf`) and return `void`. They communicate success/failure back to user space by writing to the trap frame's EAX register (via `syscall_set_errno(tf, E_SUCC)` or `syscall_set_errno(tf, E_INVAL_SIGNUM)`, etc.).

---

## 5. Subsection 5.3 — TSyscall.c: The Four Signal Syscalls

This is where the actual syscall logic lives. Each function handles one signal-related system call.

### 5.1 `sys_sigaction` — Registering a Signal Handler

**What it does:** Associates a handler function with a signal number for the calling process. This is how user code says "when signal X arrives, run function Y."

**POSIX equivalent:** `sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)`

**User-side call:**
```c
struct sigaction sa;
sa.sa_handler = my_handler;  // function pointer
sigaction(SIGINT, &sa, NULL);
```

**Full kernel implementation:**
```c
void sys_sigaction(tf_t *tf)
{
    int signum = syscall_get_arg2(tf);                                    // EBX
    struct sigaction *user_act = (struct sigaction *)syscall_get_arg3(tf); // ECX
    struct sigaction *user_oldact = (struct sigaction *)syscall_get_arg4(tf); // EDX
    unsigned int cur_pid = get_curid();
    struct sigaction kern_act;  // local kernel-space copy

    // Validate signal number
    if (signum < 1 || signum >= NSIG) {
        syscall_set_errno(tf, E_INVAL_SIGNUM);
        return;
    }

    // Save old handler if requested
    if (user_oldact != NULL) {
        struct sigaction *cur_act = tcb_get_sigaction(cur_pid, signum);
        if (cur_act != NULL) {
            pt_copyout((void*)cur_act, cur_pid, (uintptr_t)user_oldact,
                       sizeof(struct sigaction));
        }
    }

    // Set new handler if provided
    if (user_act != NULL) {
        pt_copyin(cur_pid, (uintptr_t)user_act, (void*)&kern_act,
                  sizeof(struct sigaction));
        tcb_set_sigaction(cur_pid, signum, &kern_act);
    }

    syscall_set_errno(tf, E_SUCC);
}
```

**Line-by-line breakdown:**

**Extracting arguments:**
```c
int signum = syscall_get_arg2(tf);    // reads EBX = signal number (e.g., 2 for SIGINT)
struct sigaction *user_act = (struct sigaction *)syscall_get_arg3(tf);    // reads ECX = user pointer to new action
struct sigaction *user_oldact = (struct sigaction *)syscall_get_arg4(tf); // reads EDX = user pointer to save old action
```
Remember: `syscall_get_arg2` = EBX = first real argument. The cast `(struct sigaction *)` just tells the compiler "treat this 32-bit integer as a pointer" — but it's a **user-space** pointer, not valid in kernel space.

**Validation:**
```c
if (signum < 1 || signum >= NSIG) {
    syscall_set_errno(tf, E_INVAL_SIGNUM);
    return;
}
```
Signal 0 is reserved (POSIX uses it for "test if process exists"). Signals >= NSIG (32) don't exist. Writing `E_INVAL_SIGNUM` into EAX tells the user code the call failed.

**Saving the old handler (optional):**
```c
if (user_oldact != NULL) {
    struct sigaction *cur_act = tcb_get_sigaction(cur_pid, signum);
    if (cur_act != NULL) {
        pt_copyout((void*)cur_act, cur_pid, (uintptr_t)user_oldact,
                   sizeof(struct sigaction));
    }
}
```
If the caller passed a non-NULL `oldact` pointer, it wants to know what handler was previously installed. We:
1. Get the current sigaction from the TCB (kernel memory)
2. Use `pt_copyout` to write it into the user's buffer at `user_oldact`

The direction is **Kernel → User** hence `pt_copyout`.

**Installing the new handler (the crucial part):**
```c
if (user_act != NULL) {
    pt_copyin(cur_pid, (uintptr_t)user_act, (void*)&kern_act,
              sizeof(struct sigaction));
    tcb_set_sigaction(cur_pid, signum, &kern_act);
}
```

This is the most important part and where a critical bug was discovered during implementation:

1. **`pt_copyin`** reads `sizeof(struct sigaction)` bytes from the user's address (`user_act`) into the kernel's local variable `kern_act`. Direction: **User → Kernel**.
2. **`tcb_set_sigaction`** stores the `kern_act` in the TCB's sigaction array for this signal.

**Why `pt_copyin` instead of direct pointer access?**

At this point in `trap()`, the kernel has already called `set_pdir_base(0)` to switch to the kernel's page table. User addresses are not mapped in the kernel's page table. If we tried `kern_act = *user_act`, we'd get a page fault. `pt_copyin` manually walks the user's page table to find the physical address and copies the data through the kernel's identity-mapped region.

```
 ┌─────────────────────────────────────────────────────────┐
 │  WHY pt_copyin IS REQUIRED                              │
 │                                                         │
 │  Kernel page table (PID 0):                             │
 │    VA 0x00000000..0x3FFFFFFF → PA 0x00000000..0x3FFFF.. │
 │    (identity mapped — VA == PA below 1GB)                │
 │    User addresses (0x40000000+) → NOT MAPPED ✗          │
 │                                                         │
 │  User page table (PID N):                               │
 │    VA 0x40000000+ → PA (wherever pages were allocated)   │
 │    user_act might be at VA 0xEFFFFF80                    │
 │                                                         │
 │  pt_copyin(pid, user_va, kern_buf, size):                │
 │    1. Walks PID N's page table to find PA of user_va     │
 │    2. Uses kernel's identity map to access that PA       │
 │    3. Copies bytes from PA into kern_buf                 │
 └─────────────────────────────────────────────────────────┘
```

### 5.2 `sys_kill` — Sending a Signal to a Process

**What it does:** Sends a signal to a target process. The signal becomes "pending" and will be delivered when the target next returns from a kernel trap.

**POSIX equivalent:** `kill(pid_t pid, int sig)`

**Full kernel implementation:**
```c
void sys_kill(tf_t *tf)
{
    int pid = syscall_get_arg2(tf);       // EBX = target process ID
    int signum = syscall_get_arg3(tf);    // ECX = signal number

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

    // SIGKILL is special — terminate immediately, cannot be caught
    if (signum == SIGKILL) {
        tcb_set_state(pid, TSTATE_DEAD);
        tqueue_remove(NUM_IDS, pid);
        tcb_set_pending_signals(pid, 0);
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
```

**Three paths through this function:**

**Path 1 — Validation failure:**
If `signum` is out of range or `pid` is invalid/dead, set an error code and return immediately. No signal is sent.

**Path 2 — SIGKILL (immediate termination):**
```c
if (signum == SIGKILL) {
    tcb_set_state(pid, TSTATE_DEAD);      // mark as dead
    tqueue_remove(NUM_IDS, pid);           // remove from ready queue
    tcb_set_pending_signals(pid, 0);       // clear all pending signals
    syscall_set_errno(tf, E_SUCC);
    return;
}
```
SIGKILL is special in POSIX — it **cannot be caught, blocked, or ignored**. There's no point setting it as "pending" and waiting for delivery. We kill the process right here:
- `tcb_set_state(pid, TSTATE_DEAD)` — marks the TCB state so the scheduler never picks this process again
- `tqueue_remove(NUM_IDS, pid)` — `NUM_IDS` is the index of the global ready queue. This removes the process from the scheduler's run list
- `tcb_set_pending_signals(pid, 0)` — zeroes out pending signals (dead processes don't need them)

**Path 3 — Normal signal (the common case):**
```c
tcb_add_pending_signal(pid, signum);
```
This sets bit `signum` in the target's pending signal bitmask:
```c
// Internally: tcb[pid].sig_state.pending_signals |= (1 << signum);
```
The signal is now "pending." It will actually be **delivered** later, in `handle_pending_signals()`, when the target process next transitions from kernel to user mode.

**Waking up sleeping processes:**
```c
if (tcb_is_sleeping(pid)) {
    void *chan = tcb_get_channel(pid);
    if (chan != NULL) {
        thread_wakeup(chan);
    }
}
```
If the target process is sleeping (e.g., it called `sys_pause` or is waiting for I/O), we need to wake it up so it can process the signal. `thread_wakeup(chan)` moves it from the sleep queue back to the ready queue. Without this, a process in `sys_pause` would never wake up to handle the signal.

### 5.3 `sys_pause` — Waiting for a Signal

**What it does:** Suspends the calling process until a signal is delivered.

**POSIX equivalent:** `pause()`

**Full kernel implementation:**
```c
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
```

**Two paths:**

**Path 1 — Signals already pending:** If there are already pending signals (bitmask is non-zero), return immediately. The signals will be delivered in `handle_pending_signals()` when we exit the trap handler.

**Path 2 — No signals pending:** Put the process to sleep:
1. `tcb_set_state(cur_pid, TSTATE_SLEEP)` — marks the process as sleeping
2. `thread_yield()` — gives up the CPU to another process

The process will wake up when `sys_kill` calls `thread_wakeup(chan)` on it (see section 5.2 above). After waking, execution continues at `syscall_set_errno(tf, E_SUCC)`, then returns through `trap()` where `handle_pending_signals()` actually delivers the signal.

### 5.4 `sys_sigreturn` — Returning from a Signal Handler

**What it does:** Restores the process's original execution context after a signal handler finishes. This is the "undo" operation — it puts `eip` and `esp` back to what they were before `deliver_signal` hijacked them.

**This syscall is NOT called by user code directly.** It's called by the trampoline code that the kernel placed on the user's stack (explained in detail in Section 6.2).

**Full kernel implementation:**
```c
void sys_sigreturn(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t saved_esp_addr, saved_eip_addr;
    uint32_t saved_esp, saved_eip;

    // Get the saved context addresses from TCB
    tcb_get_signal_context(cur_pid, &saved_esp_addr, &saved_eip_addr);

    if (saved_esp_addr == 0 || saved_eip_addr == 0) {
        syscall_set_errno(tf, E_INVAL_ADDR);
        return;
    }

    // Read saved ESP from user stack
    if (pt_copyin(cur_pid, saved_esp_addr, &saved_esp, sizeof(uint32_t))
            != sizeof(uint32_t)) {
        syscall_set_errno(tf, E_MEM);
        return;
    }

    // Read saved EIP from user stack
    if (pt_copyin(cur_pid, saved_eip_addr, &saved_eip, sizeof(uint32_t))
            != sizeof(uint32_t)) {
        syscall_set_errno(tf, E_MEM);
        return;
    }

    // Clear the signal context in TCB
    tcb_clear_signal_context(cur_pid);

    // Restore the original trapframe
    tf->esp = saved_esp;
    tf->eip = saved_eip;

    syscall_set_errno(tf, E_SUCC);
}
```

**Step-by-step breakdown:**

**Step 1 — Find where the original context is stored:**
```c
tcb_get_signal_context(cur_pid, &saved_esp_addr, &saved_eip_addr);
```
During `deliver_signal`, the kernel saved two **addresses** (locations on the user stack) into the TCB. These addresses tell us where the original `esp` and `eip` values were stashed. The function writes these addresses into `saved_esp_addr` and `saved_eip_addr`.

**Step 2 — Validate:**
```c
if (saved_esp_addr == 0 || saved_eip_addr == 0) { /* error */ }
```
If no signal was being handled (addresses are zero), someone called `sigreturn` incorrectly.

**Step 3 — Read the original values from user memory:**
```c
pt_copyin(cur_pid, saved_esp_addr, &saved_esp, sizeof(uint32_t));
pt_copyin(cur_pid, saved_eip_addr, &saved_eip, sizeof(uint32_t));
```
This reads the **actual saved register values** from the user stack. Remember, `deliver_signal` wrote the original `esp` and `eip` onto the user stack and then stored the *addresses of those stack locations* in the TCB. Now we follow those addresses to get the values.

```
 TCB stores:                        User stack contains:
 saved_esp_addr = 0xEFFFFF90   --→  [0xEFFFFF90]: 0xEFFFFFA0  (original ESP)
 saved_eip_addr = 0xEFFFFF94   --→  [0xEFFFFF94]: 0x00400123  (original EIP)
```

Why this two-level indirection? Because the kernel can't hold user data directly in the TCB across context switches — it stores the *address* where the data lives in the user's own address space.

**Step 4 — Restore and clean up:**
```c
tcb_clear_signal_context(cur_pid);  // reset TCB's saved addresses to 0
tf->esp = saved_esp;                 // restore original stack pointer
tf->eip = saved_eip;                 // restore original instruction pointer
```

When `trap_return` fires, the CPU will resume at the **original** `eip` with the **original** `esp`, as if the signal handler never ran.

---

## 6. Subsection 5.4 — TTrapHandler.c: The Signal Delivery Engine

This is the most complex and interesting part. Three new functions (`deliver_signal`, `terminate_process`, `handle_pending_signals`) plus a modification to the existing `trap()` function.

### 6.1 `deliver_signal` — Constructing the Signal Stack Frame

**What it does:** Modifies the trap frame so that when the CPU returns to user mode, it jumps to the signal handler instead of the original code — **and** sets up the user stack so the handler can properly receive its argument and eventually trigger `sigreturn`.

This is the most complex function in the entire signal implementation. Let's break it down piece by piece.

**Overview of what gets built on the user stack:**

```
 BEFORE deliver_signal:              AFTER deliver_signal:

 User Stack (growing down ↓)         User Stack (growing down ↓)

 ┌──────────────────────┐            ┌──────────────────────┐
 │  ... user data ...   │            │  ... user data ...   │
 │                      │            │                      │
 │                      │  orig_esp→ ├──────────────────────┤
 │                      │            │  saved_eip (4 bytes) │ ← saved_eip_addr
 │                      │            ├──────────────────────┤
 │                      │            │  saved_esp (4 bytes) │ ← saved_esp_addr
 │                      │            ├──────────────────────┤
 │                      │            │  trampoline (12 B)   │ ← trampoline_addr
 │                      │            │  B8 XX 00 00 00      │   (executable code!)
 │                      │            │  CD 30               │
 │                      │            │  EB FE               │
 │                      │            │  90 90 90            │
 │                      │            ├──────────────────────┤
 │                      │            │  signum (4 bytes)    │ ← handler's argument
 │                      │            ├──────────────────────┤
 │                      │            │  trampoline_addr     │ ← "return address"
 │                      │            │  (4 bytes)           │   for handler
 ├──────────────────────┤  new_esp→  ├──────────────────────┤
 │                      │            │                      │

 tf->eip = original code             tf->eip = sa->sa_handler
 tf->esp = orig_esp                   tf->esp = new_esp
```

**The code, annotated section by section:**

```c
static void deliver_signal(tf_t *tf, int signum)
{
    unsigned int cur_pid = get_curid();
    struct sigaction *sa = tcb_get_sigaction(cur_pid, signum);
```
Get the registered handler for this signal. `sa->sa_handler` is the function pointer the user registered via `sigaction()`.

```c
    if (sa != NULL && sa->sa_handler != NULL) {
```
Only proceed if a handler was registered. If no handler is set, the signal is silently ignored.

**Saving the original context:**
```c
        uint32_t orig_esp = tf->esp;    // where user stack was
        uint32_t orig_eip = tf->eip;    // where user code was executing
        uint32_t new_esp = orig_esp;    // we'll decrement this as we push
        size_t copied;

        // Push saved_eip (for sigreturn to read later)
        new_esp -= 4;
        copied = pt_copyout(&orig_eip, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_eip_addr = new_esp;

        // Push saved_esp (for sigreturn to read later)
        new_esp -= 4;
        copied = pt_copyout(&orig_esp, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_esp_addr = new_esp;
```

We write the original `eip` and `esp` onto the user stack. These values will be read back by `sys_sigreturn` later. We also remember the *addresses* where we stored them (that's what goes into the TCB).

**Why write to the user stack and not just save in the TCB?**
We could store the values directly in the TCB. But storing them on the user stack follows the same pattern as real operating systems: the signal frame is a user-visible data structure. This keeps the TCB small and supports future features like nested signals.

**Pushing the trampoline code:**
```c
        uint8_t trampoline[12] = {
            0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,  // mov eax, SYS_sigreturn
            0xCD, 0x30,                              // int 0x30
            0xEB, 0xFE,                              // jmp $ (infinite loop)
            0x90, 0x90, 0x90                         // nop padding
        };
        new_esp -= 12;
        copied = pt_copyout(trampoline, cur_pid, new_esp, 12);
        uint32_t trampoline_addr = new_esp;
```

This is raw machine code written onto the user stack! It will be executed as code. See Section 6.2 for the full byte-by-byte explanation.

**Setting up the cdecl call frame:**
```c
        // Align stack to 4 bytes
        new_esp = new_esp & ~3;

        // Push signal number (the argument to the handler)
        new_esp -= 4;
        uint32_t sig_arg = signum;
        copied = pt_copyout(&sig_arg, cur_pid, new_esp, sizeof(uint32_t));

        // Push return address (trampoline address)
        new_esp -= 4;
        copied = pt_copyout(&trampoline_addr, cur_pid, new_esp, sizeof(uint32_t));
```

This is **critical** and where a major bug was discovered. The x86 **cdecl calling convention** says:

```
 When a function is called with CALL instruction:

   [ESP]     → return address  (pushed by CALL)
   [ESP + 4] → first argument
   [ESP + 8] → second argument
   ...
```

Since we're not using a `CALL` instruction (we're directly setting `tf->eip`), we have to manually build what `CALL` would have created:

1. **Return address at `[ESP]`**: Set to `trampoline_addr` — when the handler does `ret`, it pops this address and jumps there, which runs the trampoline (which triggers `sigreturn`).
2. **Signal number at `[ESP + 4]`**: The handler's first (and only) argument. The handler prototype is `void handler(int signum)`, and cdecl says first argument is at `[ESP + 4]`.

**Why NOT pass signum in a register?**

This was a bug we hit! The initial implementation put the signal number in EAX thinking the handler would read it from there. But the handler is compiled C code, and C functions expect arguments on the **stack** per the cdecl convention. The handler's compiled code does something like `mov eax, [ebp+8]` to read its first argument — which reads from the stack, not a register. So we **must** put signum on the stack.

**Updating the trap frame:**
```c
        tf->esp = new_esp;                      // user stack now points to our constructed frame
        tf->eip = (uint32_t)sa->sa_handler;     // user will resume at the handler function

        tcb_set_signal_context(cur_pid, saved_esp_addr, saved_eip_addr);
```

These two lines are the actual "hijack":
- `tf->eip = sa->sa_handler` — when `trap_return` runs, CPU will jump to the handler instead of where the user code was
- `tf->esp = new_esp` — when `trap_return` runs, the user's stack will have our constructed frame on top
- `tcb_set_signal_context` — saves the stack addresses into the TCB so `sys_sigreturn` can find them later

**What the handler "sees" when it starts running:**

From the handler's perspective, it was "called" normally. The stack looks like any function call. It has a return address at `[ESP]` and its argument at `[ESP + 4]`. It does `push ebp; mov ebp, esp` (standard function prologue), accesses `signum` via `[ebp+8]`, does its work, and then does `pop ebp; ret`. The `ret` instruction pops the return address (which is `trampoline_addr`) and jumps there.

### 6.2 The Trampoline: 12 Bytes That Make Everything Work

The trampoline is the most elegant (and most mysterious-looking) part of the entire signal implementation. It's **raw x86 machine code** that the kernel writes onto the user's stack. When the signal handler does `ret`, execution jumps to this code, which triggers `sys_sigreturn` to restore the original context.

**The 12 bytes:**
```c
uint8_t trampoline[12] = {
    0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,   // mov eax, SYS_sigreturn
    0xCD, 0x30,                                // int 0x30
    0xEB, 0xFE,                                // jmp $ (infinite loop)
    0x90, 0x90, 0x90                           // nop padding
};
```

Let's decode every single byte:

---

**Bytes 0-4: `B8 XX 00 00 00` — `mov eax, imm32`**

```
 Byte 0:  B8  ← opcode for "MOV EAX, imm32"
 Bytes 1-4: XX 00 00 00  ← 32-bit immediate value (little-endian)
```

`B8` is the x86 opcode for "load a 32-bit immediate value into EAX." The next 4 bytes are the value in **little-endian** format.

`SYS_sigreturn` is 27 (0x1B). In little-endian, 27 is stored as `1B 00 00 00`. So the actual bytes are:

```
 B8 1B 00 00 00
```

This instruction sets `EAX = 27`, which is the syscall number for sigreturn. Remember from Section 2.2: the syscall number goes in EAX.

**Why `B8` and not `MOV`?** `B8` **is** `MOV`. In x86 machine code, `B8` is one of the "MOV register, immediate" opcodes. There's no human-readable assembler here — we're writing raw opcodes because this code lives on the stack and was constructed at runtime.

```
 x86 opcode table for MOV r32, imm32:
   B8 = MOV EAX
   B9 = MOV ECX
   BA = MOV EDX
   BB = MOV EBX
   BC = MOV ESP
   BD = MOV EBP
   BE = MOV ESI
   BF = MOV EDI
```

---

**Bytes 5-6: `CD 30` — `int 0x30`**

```
 Byte 5:  CD  ← opcode for "INT imm8" (software interrupt)
 Byte 6:  30  ← interrupt number 0x30 = 48 decimal = T_SYSCALL
```

`CD` is the x86 opcode for a software interrupt. `30` is the interrupt number (48 in decimal), which is CertiKOS's syscall interrupt vector (`T_SYSCALL`). This triggers a trap into the kernel.

Since EAX was set to `SYS_sigreturn` (27) by the previous instruction, the kernel will dispatch to `sys_sigreturn`, which restores the original context.

**The full effect:**
```
 After handler returns (via RET), execution lands here:

   mov eax, 27        ; prepare syscall number
   int 0x30           ; TRAP! → kernel → sys_sigreturn → restore original state
```

---

**Bytes 7-8: `EB FE` — `jmp $` (infinite loop safety net)**

```
 Byte 7:  EB  ← opcode for "JMP rel8" (short relative jump)
 Byte 8:  FE  ← signed offset = -2
```

`EB` is a short relative jump. The next byte is a **signed 8-bit displacement** from the address *after* the instruction. Since this jmp instruction is 2 bytes long:

```
 Address of EB FE:  let's call it A
 Address after instruction: A + 2
 Jump target: (A + 2) + (-2) = A
 → jumps back to itself!
```

$FE$ in signed 8-bit = $-2$ (because $\text{FE}_{16} = 254_{10}$, and as signed: $254 - 256 = -2$).

This creates an infinite loop: `jmp $` in assembly syntax, meaning "jump to the current address." This is a **safety net**: if `int 0x30` somehow returns (it shouldn't — `sys_sigreturn` redirects `eip` elsewhere), the CPU spins here instead of falling through into whatever junk is below on the stack.

In practice, `int 0x30` will never return here because `sys_sigreturn` changes `tf->eip` to the original code, and `trap_return` sends the CPU there.

---

**Bytes 9-11: `90 90 90` — NOP padding**

```
 Byte 9:   90 ← NOP (no operation)
 Byte 10:  90 ← NOP
 Byte 11:  90 ← NOP
```

`0x90` is the x86 NOP (No Operation) instruction. These three bytes pad the trampoline to 12 bytes for alignment. On x86, 12 bytes is a multiple of 4, which keeps the stack 4-byte aligned. These bytes are never executed (the infinite loop above prevents reaching them).

---

**The trampoline as assembly:**

Converting the raw bytes back to human-readable assembly:

```asm
; Trampoline code (lives on user stack)
trampoline:
    mov eax, 27          ; B8 1B 00 00 00   — set syscall number to SYS_sigreturn
    int 0x30             ; CD 30             — trap into kernel (syscall)
    jmp trampoline+5     ; EB FE             — safety: spin forever if we return
                         ;                     (actually jumps to itself, jmp $)
    nop                  ; 90                — padding
    nop                  ; 90                — padding
    nop                  ; 90                — padding
```

**Why is the trampoline on the stack and not in a fixed location?**

In real operating systems like Linux, the trampoline (called `__kernel_sigreturn`) lives in a special page (the vDSO) mapped into every process. In mCertiKOS, we don't have a vDSO, so we put the trampoline directly on the stack. This is the simplest approach but requires that the stack is executable (which it is in mCertiKOS — there's no NX bit enforcement).

### 6.3 `terminate_process` — The SIGKILL Path

**Full implementation:**
```c
static void terminate_process(unsigned int pid)
{
    tcb_set_state(pid, TSTATE_DEAD);         // mark as dead
    tqueue_remove(NUM_IDS, pid);              // remove from ready queue
    tcb_set_pending_signals(pid, 0);          // clear pending signals
}
```

This is the SIGKILL handler called from `handle_pending_signals`. It's almost identical to the SIGKILL path in `sys_kill`, but exists separately because:

- `sys_kill` handles SIGKILL from the **sender** side (process A kills process B)
- `terminate_process` handles SIGKILL from the **receiver** side (process B is processing its own pending SIGKILL)

Both end the same way: `TSTATE_DEAD`, removed from queue, signals cleared.

### 6.4 `handle_pending_signals` — The Signal Check Loop

**What it does:** Checks if the current process has any pending signals. If so, picks the lowest-numbered one and either delivers it (regular signal) or terminates the process (SIGKILL).

```c
static void handle_pending_signals(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t pending_signals = tcb_get_pending_signals(cur_pid);

    if (pending_signals != 0) {
        for (int signum = 1; signum < NSIG; signum++) {
            if (pending_signals & (1 << signum)) {
                // Clear the pending signal
                tcb_clear_pending_signal(cur_pid, signum);

                // SIGKILL cannot be caught
                if (signum == SIGKILL) {
                    terminate_process(cur_pid);
                    thread_exit();
                    return;
                }

                // Deliver the signal to handler
                deliver_signal(tf, signum);
                break;   // ← only deliver ONE signal per trap return
            }
        }
    }
}
```

**Key design decisions:**

**1. Check from signal 1 upward — lowest number wins:**
```c
for (int signum = 1; signum < NSIG; signum++)
```
If multiple signals are pending (e.g., both SIGINT=2 and SIGTERM=15), the lowest-numbered one gets delivered first. The loop starts at 1 (signal 0 is unused) and stops at `NSIG-1` (31).

**2. Clear before deliver:**
```c
tcb_clear_pending_signal(cur_pid, signum);
```
We clear the pending bit **before** calling `deliver_signal`. This prevents the same signal from being delivered again on the next trap return. Internally this does:
```c
pending_signals &= ~(1 << signum);   // turn OFF bit 'signum'
```

**3. SIGKILL → terminate, not deliver:**
```c
if (signum == SIGKILL) {
    terminate_process(cur_pid);
    thread_exit();          // ← NOT thread_yield()!
    return;
}
```
SIGKILL bypasses the handler entirely. We terminate and call `thread_exit()`.

**Why `thread_exit()` and not `thread_yield()`?** This was a bug we discovered. `thread_yield()` puts the process back into the ready queue — but we just marked it as `TSTATE_DEAD`. If it goes back in the queue, the scheduler might try to run a dead process. `thread_exit()` properly removes it without re-enqueueing.

**4. Break after first delivery — one signal per trap:**
```c
deliver_signal(tf, signum);
break;
```
We only deliver one signal per trap return. If multiple signals are pending, the others will be delivered on subsequent trap returns. This simplifies the implementation (no nested signal frames) and matches how many real OSes handle it.

### 6.5 The Modified `trap()` Function — The Critical Ordering

The `trap()` function is the master handler for all traps. Here's the modified version with the signal check:

```c
void trap(tf_t *tf)
{
    unsigned int cur_pid;

    cur_pid = get_curid();
    set_pdir_base(0);           // ① switch to kernel page table

    trap_cb_t f;
    f = TRAP_HANDLER[get_pcpu_idx()][tf->trapno];

    if (f) {
        f(tf);                  // ② dispatch to handler (e.g., syscall_dispatch)
    } else {
        KERN_WARN("No handler for trap 0x%x\n", tf->trapno);
    }

    kstack_switch(cur_pid);     // ③ switch to user's kernel stack

    // ④ Check signals BEFORE switching page tables
    handle_pending_signals(tf);

    set_pdir_base(cur_pid);     // ⑤ switch to user's page table

    trap_return(tf);            // ⑥ return to user mode
}
```

**The ordering of steps ④ and ⑤ is THE MOST CRITICAL detail in the entire signal implementation.** Getting this wrong was the hardest bug to find.

**Why must `handle_pending_signals` come BEFORE `set_pdir_base(cur_pid)`?**

`handle_pending_signals` → `deliver_signal` → `pt_copyout` — this chain needs to write data onto the user's stack. `pt_copyout` works by:

1. Walking the user's page table to translate virtual address → physical address
2. Using the **kernel's identity mapping** (where VA = PA) to write to that physical address

Step 2 **only works under the kernel's page table** (PID 0), where physical addresses are identity-mapped. If we've already switched to the user's page table (`set_pdir_base(cur_pid)`), the kernel can't use identity mapping to access physical memory, and `pt_copyout` will fail or corrupt memory.

```
 ✗ WRONG ORDER (causes page faults or corruption):

    set_pdir_base(cur_pid);      // switch to user PT
    handle_pending_signals(tf);  // pt_copyout tries identity mapping → FAILS
    trap_return(tf);

 ✓ CORRECT ORDER:

    handle_pending_signals(tf);  // pt_copyout uses identity mapping → works
    set_pdir_base(cur_pid);      // NOW switch to user PT
    trap_return(tf);             // return to user mode with user PT active
```

```
 ┌──────────────────────────────────────────────────────────────┐
 │  WHY THE ORDER MATTERS — PAGE TABLE DIAGRAM                 │
 │                                                              │
 │  Kernel PT (PID 0):     User PT (PID N):                    │
 │  VA 0x00000000 → PA 0   VA 0x40000000 → PA 0x300000        │
 │  VA 0x00001000 → PA 1   VA 0x40001000 → PA 0x500000        │
 │  VA 0x00002000 → PA 2   ...                                 │
 │  ...   (identity map)   VA 0xEFFFFF80 → PA 0x200000 (stack)│
 │  VA = PA for all         VA 0x00000000 → NOT MAPPED ✗       │
 │                                                              │
 │  pt_copyout translates VA 0xEFFFFF80 → PA 0x200000          │
 │  Then writes to VA 0x200000 (which = PA 0x200000             │
 │  ONLY under kernel PT!)                                      │
 │                                                              │
 │  Under user PT, VA 0x200000 is NOT MAPPED → PAGE FAULT      │
 └──────────────────────────────────────────────────────────────┘
```

---

## 7. The `trap_return` Assembly — How the CPU Resumes User Code

After all signal processing is done, `trap()` calls `trap_return(tf)` to send the CPU back to user mode. This function is written in assembly (`kern/dev/idt.S`) because it needs to manipulate the CPU state at a level C cannot reach.

**The full assembly:**

```asm
.globl  trap_return
.type   trap_return,@function
.p2align 4, 0x90            ; align to 16-byte boundary, fill with NOPs
trap_return:
    movl    4(%esp), %esp    ; (1) reset stack pointer to trap frame
    popal                    ; (2) restore general-purpose registers
    popl    %es              ; (3) restore ES segment register
    popl    %ds              ; (4) restore DS segment register
    addl    $8, %esp         ; (5) skip trapno and error code
    iret                     ; (6) return from interrupt
```

Let's understand each instruction in detail:

---

**Instruction 1: `movl 4(%esp), %esp`**

```
 Before:  ESP points to trap_return's own stack frame
          [ESP]     = return address (where trap() called us from)
          [ESP + 4] = first argument = pointer to tf_t struct

 After:   ESP = address of the tf_t struct
```

`trap_return` was called as a C function: `trap_return(tf)`. In cdecl, the argument `tf` is at `[ESP + 4]` (because `[ESP]` is the return address pushed by `CALL`). This instruction loads `tf` into ESP, so now ESP points directly at the beginning of the trap frame struct.

**Why not just `mov esp, [ebp+8]`?** This function never sets up a stack frame (no `push ebp; mov ebp, esp`). It's a "naked" function that goes straight to work. Since it never returns (it uses `iret`), there's no need for a frame pointer.

**After this instruction, the stack looks like the tf_t struct layout:**
```
 ESP+0:   regs.edi     ┐
 ESP+4:   regs.esi     │
 ESP+8:   regs.ebp     │
 ESP+12:  regs.oesp    ├── pushregs (32 bytes)
 ESP+16:  regs.ebx     │
 ESP+20:  regs.edx     │
 ESP+24:  regs.ecx     │
 ESP+28:  regs.eax     ┘
 ESP+32:  es (+ padding)    ← 4 bytes
 ESP+36:  ds (+ padding)    ← 4 bytes
 ESP+40:  trapno             ← 4 bytes
 ESP+44:  err                ← 4 bytes
 ESP+48:  eip           ┐
 ESP+52:  cs             │
 ESP+56:  eflags         ├── pushed by CPU on trap entry
 ESP+60:  esp            │   (popped by IRET)
 ESP+64:  ss             ┘
```

---

**Instruction 2: `popal`**

```
 POP All Long — pops 8 × 32-bit values into registers:
   EDI ← [ESP],     ESP += 4
   ESI ← [ESP],     ESP += 4
   EBP ← [ESP],     ESP += 4
   (skip ESP value), ESP += 4    ← oesp is discarded
   EBX ← [ESP],     ESP += 4
   EDX ← [ESP],     ESP += 4
   ECX ← [ESP],     ESP += 4
   EAX ← [ESP],     ESP += 4
```

This restores all 8 general-purpose registers from the `pushregs` part of the trap frame. The order matches because the struct was filled by `pushal` during trap entry (which pushes in the reverse order).

Note: `popal` skips the stored ESP value (called `oesp` in the struct). This is a quirk of `popal` — it reads the value from the stack position but doesn't load it into ESP. ESP is restored by `iret` later.

**After `popal`: ESP has advanced 32 bytes, now pointing at `es`.**

**For signal delivery, this is where EAX gets the errno value.** `syscall_set_errno(tf, E_SUCC)` earlier set `tf->regs.eax = 0`, so `popal` loads `EAX = 0` (success).

---

**Instruction 3: `popl %es`**

```
 ES ← [ESP]   (the bottom 16 bits; top 16 are padding)
 ESP += 4
```

Restores the Extra Segment register. In user mode, this is typically set to the user data segment selector (`CPU_GDT_UDATA | 3 = 0x23`).

---

**Instruction 4: `popl %ds`**

```
 DS ← [ESP]   (the bottom 16 bits)
 ESP += 4
```

Restores the Data Segment register. Same value as ES in user mode.

**After instructions 3-4: ESP has advanced 8 more bytes, now pointing at `trapno`.**

---

**Instruction 5: `addl $8, %esp`**

```
 ESP += 8
 Skips over: trapno (4 bytes) + err (4 bytes)
```

The trap number and error code are not needed when returning — they were only relevant for identifying which trap occurred. We skip them because the next instruction (`iret`) expects ESP to point at the hardware-defined return frame: `eip, cs, eflags, esp, ss`.

**After this instruction: ESP points at `eip`, which is the start of the IRET frame.**

```
 ESP+0:  eip      ← instruction to resume at
 ESP+4:  cs       ← code segment (user = 0x1B)
 ESP+8:  eflags   ← flags to restore
 ESP+12: esp      ← user stack pointer to restore
 ESP+16: ss       ← stack segment (user = 0x23)
```

---

**Instruction 6: `iret` — Interrupt Return**

```
 IRET performs an atomic sequence:
   1. POP EIP  ← where to resume executing
   2. POP CS   ← which code segment (determines privilege level)
   3. POP EFLAGS ← restore flags (including interrupt enable)
   4. If crossing privilege levels (Ring 0 → Ring 3):
      5. POP ESP ← restore user stack pointer
      6. POP SS  ← restore user stack segment
   7. CPU is now in Ring 3, executing user code
```

This is the magic instruction that sends the CPU back to user mode. It's the **reverse** of what happened when `int 0x30` was executed. The CPU atomically:
- Loads EIP from the stack (this is where `deliver_signal` put the handler address!)
- Loads CS from the stack (the user code segment selector, Ring 3)
- Loads EFLAGS (restoring interrupt enabled/disabled state)
- Detects a privilege level change (Ring 0 → Ring 3), so also:
- Loads ESP from the stack (this is where `deliver_signal` put the new stack pointer!)
- Loads SS from the stack (user stack segment)

**For signal delivery, the values IRET pops are:**
- `EIP = sa->sa_handler` — jumps to the signal handler
- `ESP = new_esp` — points at the constructed call frame (return addr + signum)

The user process resumes execution at the handler function, with the signal number on the stack and a return address pointing to the trampoline. The user has no idea any of this happened.

---

**The complete `trap_return` flow, visualized:**

```
 ESP at entry: points to C stack
       │
       ▼
 ┌─ movl 4(%esp), %esp ──→ ESP = &tf (trap frame)
 │
 ├─ popal ────────────────→ EDI,ESI,EBP,_,EBX,EDX,ECX,EAX restored
 │                          ESP advanced by 32
 │
 ├─ popl %es ─────────────→ ES restored, ESP += 4
 │
 ├─ popl %ds ─────────────→ DS restored, ESP += 4
 │
 ├─ addl $8, %esp ────────→ skip trapno + err, ESP += 8
 │
 └─ iret ─────────────────→ POP eip, cs, eflags, esp, ss
                             CPU switches to Ring 3
                             Execution continues at eip
                             Stack is at esp

 Total effect: CPU state = exactly what's in the trap frame
               If deliver_signal modified tf->eip and tf->esp,
               the CPU goes to the handler with the crafted stack
```

---

## 8. End-to-End Walkthrough: Signal Lifecycle

Let's trace a complete signal delivery from start to finish. Assume:
- Process 3 is running user code at address `0x00400100`
- Process 3 registered a handler for SIGINT (signal 2) at address `0x00400500`
- Process 2 calls `kill(3, SIGINT)` (or the shell sends it)

```
 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 1: SIGNAL REGISTRATION (earlier)                     │
 │                                                              │
 │  Process 3 calls:                                            │
 │    struct sigaction sa = { .sa_handler = my_handler };       │
 │    sigaction(SIGINT, &sa, NULL);                             │
 │                                                              │
 │  User code:                                                  │
 │    mov eax, SYS_sigaction    ; EAX = 24                     │
 │    mov ebx, 2                ; EBX = SIGINT                  │
 │    mov ecx, &sa              ; ECX = user pointer to sa      │
 │    mov edx, 0                ; EDX = NULL (no oldact)        │
 │    int 0x30                  ; trap into kernel               │
 │                                                              │
 │  Kernel:                                                     │
 │    sys_sigaction:                                             │
 │    ├─ pt_copyin(&sa → kern_act)    ← copy sa from user      │
 │    └─ tcb_set_sigaction(3, 2, &kern_act) ← store in TCB     │
 └──────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 2: SIGNAL SENDING                                    │
 │                                                              │
 │  Process 2 calls:                                            │
 │    kill(3, SIGINT);                                          │
 │                                                              │
 │  Kernel:                                                     │
 │    sys_kill:                                                  │
 │    ├─ validate pid=3 (alive ✓) and signum=2 (valid ✓)       │
 │    ├─ signum != SIGKILL, so normal path                      │
 │    ├─ tcb_add_pending_signal(3, 2)                           │
 │    │   → pending_signals |= (1 << 2) = 0x00000004           │
 │    └─ if process 3 sleeping → wake it up                     │
 │                                                              │
 │  Signal is now PENDING in process 3's TCB.                   │
 │  Process 3 is not running yet — it will be scheduled later.  │
 └──────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 3: SIGNAL DELIVERY (on next trap return for pid 3)   │
 │                                                              │
 │  Process 3 gets scheduled and runs.                          │
 │  Eventually it traps into the kernel (syscall, timer, etc.)  │
 │  trap() runs:                                                │
 │    ① set_pdir_base(0)           ← kernel page table          │
 │    ② f(tf)                      ← handle the trap            │
 │    ③ kstack_switch(3)                                         │
 │    ④ handle_pending_signals(tf) ← HERE IS WHERE IT HAPPENS   │
 │       ├─ pending = 0x00000004 ≠ 0 → signals pending!         │
 │       ├─ for signum=1: bit 1 not set, skip                   │
 │       ├─ for signum=2: bit 2 IS SET → process this one       │
 │       ├─ tcb_clear_pending_signal(3, 2)                      │
 │       │   → pending &= ~(1<<2), now pending = 0x00000000     │
 │       ├─ signum=2 ≠ SIGKILL → not terminated                 │
 │       └─ deliver_signal(tf, 2)                                │
 │           ├─ sa = tcb_get_sigaction(3, 2)                     │
 │           │   → sa->sa_handler = 0x00400500                   │
 │           ├─ orig_esp = tf->esp (e.g., 0xEFFFFFA0)           │
 │           ├─ orig_eip = tf->eip (e.g., 0x00400100)           │
 │           ├─ push saved_eip → user stack at 0xEFFFFF9C        │
 │           ├─ push saved_esp → user stack at 0xEFFFFF98        │
 │           ├─ push trampoline → user stack at 0xEFFFFF8C       │
 │           ├─ push signum (2) → user stack at 0xEFFFFF88       │
 │           ├─ push trampoline_addr → user stack at 0xEFFFFF84  │
 │           ├─ tf->esp = 0xEFFFFF84 (new stack top)             │
 │           ├─ tf->eip = 0x00400500 (handler address)           │
 │           └─ tcb_set_signal_context(3, 0xEFFFFF98, 0xEFFFFF9C)│
 │    ⑤ set_pdir_base(3)          ← user page table             │
 │    ⑥ trap_return(tf)           ← IRET with modified tf       │
 │                                                               │
 │  trap_return pops:                                            │
 │    EIP = 0x00400500 (handler!)                                │
 │    ESP = 0xEFFFFF84 (crafted stack)                           │
 │    CPU is now in Ring 3, executing the signal handler         │
 └──────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 4: HANDLER EXECUTION (user mode)                     │
 │                                                              │
 │  Handler starts at 0x00400500.                               │
 │  Stack at entry:                                             │
 │    [ESP]   = 0xEFFFFF8C (trampoline addr = return address)   │
 │    [ESP+4] = 2 (signum = SIGINT)                             │
 │                                                              │
 │  Compiled handler code (from C):                             │
 │    void my_handler(int signum) {                             │
 │        // signum is at [EBP+8] = [ESP+4] at entry            │
 │        printf("Got signal %d\n", signum);                    │
 │    }                                                         │
 │                                                              │
 │  Handler does:                                               │
 │    push ebp                                                  │
 │    mov ebp, esp                                              │
 │    ... function body ...                                     │
 │    pop ebp                                                   │
 │    ret            ← pops return address = 0xEFFFFF8C         │
 │                     jumps to trampoline on the stack!         │
 └──────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 5: TRAMPOLINE EXECUTION (user mode)                  │
 │                                                              │
 │  CPU is now executing code at 0xEFFFFF8C (on the stack!)    │
 │                                                              │
 │    B8 1B 00 00 00    →  mov eax, 27     (SYS_sigreturn)     │
 │    CD 30             →  int 0x30        (syscall!)           │
 │                                                              │
 │  Trap into kernel → EAX = 27 → syscall_dispatch →           │
 │  case SYS_sigreturn → sys_sigreturn(tf)                     │
 └──────────────────────────────────────────────────────────────┘

 ┌──────────────────────────────────────────────────────────────┐
 │  PHASE 6: CONTEXT RESTORATION (kernel mode)                 │
 │                                                              │
 │  sys_sigreturn:                                              │
 │    ├─ tcb_get_signal_context(3) → saved_esp_addr=0xEFFFFF98 │
 │    │                               saved_eip_addr=0xEFFFFF9C│
 │    ├─ pt_copyin(saved_esp_addr) → saved_esp = 0xEFFFFFA0    │
 │    ├─ pt_copyin(saved_eip_addr) → saved_eip = 0x00400100    │
 │    ├─ tcb_clear_signal_context(3)                            │
 │    ├─ tf->esp = 0xEFFFFFA0   ← ORIGINAL stack pointer       │
 │    └─ tf->eip = 0x00400100   ← ORIGINAL instruction pointer │
 │                                                              │
 │  trap() continues → trap_return(tf)                          │
 │  IRET pops: EIP=0x00400100, ESP=0xEFFFFFA0                  │
 │                                                              │
 │  Process 3 resumes EXACTLY where it was before the signal.   │
 │  It has no idea a signal handler ran.                        │
 └──────────────────────────────────────────────────────────────┘
```

---

## 9. Common Pitfalls and Lessons Learned

These are bugs that were actually encountered during the implementation of this signal system. Each one cost significant debugging time.

### Pitfall 1: Direct User Pointer Access vs. `pt_copyin`

**Bug:** Early `sys_sigaction` did `tcb_set_sigaction(cur_pid, signum, user_act)` — passing the user pointer directly to the TCB. This stored a user-space pointer in the kernel, which is meaningless under a different page table.

**Fix:** Use `pt_copyin` to copy the struct into a kernel-local variable first, then store the kernel copy in the TCB.

### Pitfall 2: Page Table Order in `trap()`

**Bug:** `handle_pending_signals` was called **after** `set_pdir_base(cur_pid)`. The `pt_copyout` inside `deliver_signal` failed because it relies on the kernel's identity mapping, which is only available under PID 0's page table.

**Fix:** Move `handle_pending_signals(tf)` to **before** `set_pdir_base(cur_pid)`.

### Pitfall 3: Passing Signal Number in Register Instead of Stack

**Bug:** Signal number was stored in a register (EAX) expecting the handler to read it from there. But the handler is a compiled C function that reads its argument from `[EBP+8]` (the stack) per cdecl.

**Fix:** Push `signum` onto the user stack at `[ESP+4]` and push the return address at `[ESP]`, creating a proper cdecl call frame.

### Pitfall 4: `thread_yield` vs. `thread_exit` for SIGKILL

**Bug:** After terminating a process via SIGKILL, `thread_yield()` was called. `thread_yield()` re-enqueues the current process in the ready queue, but we just marked it `TSTATE_DEAD`. The scheduler would pick it up and try to run a dead process.

**Fix:** Use `thread_exit()` instead, which does not re-enqueue the process.

### Pitfall 5: Trampoline Syscall Number Mismatch

**Bug:** If `SYS_sigreturn` in the trampoline byte `0xB8, SYS_sigreturn, ...` doesn't match the kernel's enum value, the kernel dispatches to the wrong syscall (or returns `E_INVAL_CALLNR`).

**Fix:** Use the `SYS_sigreturn` constant directly in the trampoline array. The compiler substitutes the correct enum value at compile time. Do NOT hardcode a number.

### Pitfall 6: Missing Handler NULL Check

**Bug:** If `deliver_signal` is called for a signal with no registered handler (`sa_handler == NULL`), dereferencing `sa->sa_handler` to set `tf->eip` would set EIP to 0, causing a page fault.

**Fix:** Check both `sa != NULL` and `sa->sa_handler != NULL` before proceeding with delivery. If no handler is registered, the signal is silently ignored.

---

## Summary

The trap layer changes (Section 5) form the complete signal pipeline:

| Component | File | Purpose |
|-----------|------|---------|
| Dispatch routing | TDispatch.c | Routes 4 new syscall numbers to their handlers |
| `sys_sigaction` | TSyscall.c | Registers handlers (with `pt_copyin`/`pt_copyout` for safe user↔kernel data transfer) |
| `sys_kill` | TSyscall.c | Sends signals (bitmask pending) or kills immediately (SIGKILL) |
| `sys_pause` | TSyscall.c | Sleeps until signal arrives |
| `sys_sigreturn` | TSyscall.c | Restores original EIP/ESP from user stack (via `pt_copyin`) |
| `deliver_signal` | TTrapHandler.c | Builds the signal frame on user stack (saved context + trampoline + cdecl frame) |
| `handle_pending_signals` | TTrapHandler.c | Checks bitmask, dispatches one signal per trap return |
| `trap()` modification | TTrapHandler.c | Inserts signal check at the correct point (before page table switch) |
| `trap_return` | idt.S | Assembly that loads tf_t into CPU state and IRETs to user mode |

The entire system works because the kernel can modify two fields in the trap frame (`tf->eip` and `tf->esp`) to redirect user execution anywhere it wants. Signal delivery is, fundamentally, a controlled hijack of the user's execution context.
