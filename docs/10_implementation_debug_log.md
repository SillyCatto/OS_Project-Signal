# Signal Implementation Debug Log

This document chronicles the complete implementation and debugging process for adding POSIX-style signal handling to CertiKOS. This was a challenging multi-day debugging session with numerous trial-and-error iterations.

## Table of Contents

1. [Overview](#overview)
2. [Implementation Summary](#implementation-summary)
3. [Bug Chronicle](#bug-chronicle)
4. [Final Working Implementation](#final-working-implementation)
5. [Testing Procedures](#testing-procedures)
6. [Lessons Learned](#lessons-learned)

---

## Overview

### Goal
Implement a working signal mechanism in CertiKOS that supports:
- Registering signal handlers via `sigaction()`
- Sending signals to processes via `kill()`
- Executing user-space signal handlers
- Returning from handlers via `sigreturn()`
- Force-terminating processes via `SIGKILL`

### Final Result
✅ All features working correctly after extensive debugging.

---

## Implementation Summary

### Files Modified

| File | Purpose | Changes |
|------|---------|---------|
| `kern/lib/signal.h` | Signal structures | Added `sig_state`, `sigaction`, signal constants |
| `kern/lib/syscall.h` | Syscall numbers | Added `SYS_sigaction`, `SYS_kill`, `SYS_pause`, `SYS_sigreturn` |
| `kern/thread/PTCBIntro/PTCBIntro.c` | TCB management | Added signal state to TCB, accessor functions |
| `kern/thread/PTCBIntro/export.h` | TCB exports | Added signal accessor declarations |
| `kern/thread/PThread/PThread.c` | Thread management | Added `thread_exit()` for process termination |
| `kern/thread/PThread/export.h` | Thread exports | Added `thread_exit()` declaration |
| `kern/trap/TSyscall/TSyscall.c` | Syscall handlers | Added `sys_sigaction`, `sys_kill`, `sys_pause`, `sys_sigreturn` |
| `kern/trap/TDispatch/TDispatch.c` | Syscall dispatch | Added cases for signal syscalls |
| `kern/trap/TDispatch/import.h` | Dispatch imports | Added signal syscall declarations |
| `kern/trap/TTrapHandler/TTrapHandler.c` | Trap handling | Added `deliver_signal`, `handle_pending_signals`, `terminate_process` |
| `user/lib/signal.c` | User-space wrappers | Added `sigaction()`, `kill()`, `pause()` |
| `user/include/signal.h` | User-space headers | Added signal types and function declarations |
| `user/shell/shell.c` | Shell commands | Added `trap`, `kill`, `spawn` commands |

### New Functions Added

#### Kernel Space

```c
// kern/trap/TSyscall/TSyscall.c
void sys_sigaction(tf_t *tf);    // Register signal handler
void sys_kill(tf_t *tf);          // Send signal to process
void sys_pause(tf_t *tf);         // Wait for signal
void sys_sigreturn(tf_t *tf);     // Return from signal handler

// kern/trap/TTrapHandler/TTrapHandler.c
static void deliver_signal(tf_t *tf, int signum);      // Set up handler execution
static void handle_pending_signals(tf_t *tf);          // Check and process signals
static void terminate_process(unsigned int pid);       // Terminate via SIGKILL

// kern/thread/PTCBIntro/PTCBIntro.c
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

// kern/thread/PThread/PThread.c
void thread_exit(void);  // Exit without re-queuing (for termination)
```

#### User Space

```c
// user/lib/signal.c
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int kill(pid_t pid, int sig);
int pause(void);

// user/shell/shell.c
int shell_kill(int argc, char **argv);   // kill -N PID
int shell_trap(int argc, char **argv);   // trap N (register handler)
int shell_spawn(int argc, char **argv);  // spawn ELF_ID
```

---

## Bug Chronicle

### Bug #1: Shell `kill` Command Not Parsing Arguments

**Symptom**: Shell command `kill 2 2` didn't work.

**Root Cause**: Shell expected `kill -N PID` format (with dash), but parsing was incorrect.

**Fix**: Updated `shell_kill()` to properly parse `-N` format:
```c
// Parse signal number - handle both "2" and "-2" formats
char *sig_str = argv[1];
if (sig_str[0] == '-') {
    sig_str++;  // Skip the dash
}
int sig = str_to_int(sig_str);
```

---

### Bug #2: Signal Syscalls Not Dispatched

**Symptom**: `sigaction()` syscall returned with no effect.

**Root Cause**: Missing cases in `TDispatch.c` switch statement.

**Fix**: Added dispatch cases:
```c
case SYS_sigaction:
    sys_sigaction(tf);
    break;
case SYS_kill:
    sys_kill(tf);
    break;
case SYS_pause:
    sys_pause(tf);
    break;
```

---

### Bug #3: Handler Address Always NULL

**Symptom**: Signal handler was registered, but `sa->sa_handler` was always NULL.

**Root Cause**: `sys_sigaction()` was directly dereferencing user-space pointer instead of copying from user space.

**Fix**: Used `pt_copyin()` to copy from user space:
```c
// WRONG - direct dereference of user pointer
tcb_set_sigaction(cur_pid, signum, user_act);

// CORRECT - copy from user space
struct sigaction kern_act;
pt_copyin(cur_pid, (uintptr_t)user_act, &kern_act, sizeof(struct sigaction));
tcb_set_sigaction(cur_pid, signum, &kern_act);
```

---

### Bug #4: Signal Handler Receives Wrong Argument (Garbage Value)

**Symptom**: Handler printed garbage like `*** Received signal 1024 ***` instead of `2`.

**Root Cause #1**: Stack setup had wrong order - return address and argument were swapped.

**Initial Wrong Stack Layout**:
```
[esp]     -> signum (argument)       <- WRONG! CPU pops this as return address
[esp+4]   -> return_addr
```

**Correct cdecl Stack Layout**:
```
[esp]     -> return_addr             <- CPU pops this on 'ret'
[esp+4]   -> signum (argument)       <- Handler accesses this as first arg
```

**Fix**: Corrected the order in `deliver_signal()`:
```c
// Push signal number (argument) FIRST
new_esp -= 4;
pt_copyout(&sig_arg, cur_pid, new_esp, sizeof(uint32_t));

// Push return address (trampoline) SECOND - this goes on top
new_esp -= 4;
pt_copyout(&trampoline_addr, cur_pid, new_esp, sizeof(uint32_t));
```

---

### Bug #5: Handler Still Receives Garbage Despite Correct Stack Order

**Symptom**: Even with correct stack order, handler received wrong values. Debug output showed correct values written to physical memory but wrong values read back.

**Root Cause**: This was the **CRITICAL BUG** that took longest to find.

The trap handler code was:
```c
void trap(tf_t *tf) {
    set_pdir_base(0);  // Switch to kernel page table
    // ... handle trap ...

    // Handle signals AFTER switching back to user page table
    set_pdir_base(cur_pid);  // Switch to user page table
    handle_pending_signals(tf);  // <-- BUG: Using user page table!
}
```

**The Problem**:
- `handle_pending_signals()` calls `deliver_signal()` which calls `pt_copyout()`
- `pt_copyout()` walks the page table to find the physical address
- But after `set_pdir_base(cur_pid)`, we're using the **user's page table**
- The kernel reads the PTE from the user's virtual address space
- User address `0x40012ffc` in user's page table maps to **user code/data**
- But in kernel's page table (process 0), high physical addresses are identity-mapped
- So the PTE lookup returned garbage because the kernel was looking at the wrong memory!

**Visual Explanation**:
```
Kernel Page Table (PID 0):
  VA 0x40012000 -> PA 0x40012000 (identity mapped, kernel data)

User Page Table (PID 2):
  VA 0x40012000 -> PA 0x???????? (user's stack or code)

When kernel reads PTE for user address from user's page table,
it gets wrong physical address!
```

**Fix**: Move signal handling BEFORE switching to user page table:
```c
void trap(tf_t *tf) {
    set_pdir_base(0);  // Switch to kernel page table
    // ... handle trap ...

    // Handle signals WHILE STILL IN KERNEL PAGE TABLE
    handle_pending_signals(tf);  // <-- FIXED: Using kernel page table!

    // NOW switch to user page table
    set_pdir_base(cur_pid);
}
```

This was the **most critical fix** - signals must be processed while the kernel page table is active so that `pt_copyout()` can correctly access physical memory through identity mapping.

---

### Bug #6: Handler Crashes on Return

**Symptom**: Handler printed correct message but crashed trying to return (page fault at `0xffffffff`).

**Root Cause**: No mechanism to return from signal handler. Handler did `ret` which popped garbage.

**Fix**: Implemented sigreturn mechanism with trampoline:

1. **Trampoline Code**: Push executable code onto user stack:
```c
uint8_t trampoline[12] = {
    0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,  // mov eax, SYS_sigreturn
    0xCD, 0x30,                              // int 0x30 (syscall)
    0xEB, 0xFE,                              // jmp $ (infinite loop - safety)
    0x90, 0x90, 0x90                         // nop padding
};
```

2. **Stack Layout for sigreturn**:
```
High Address
+------------------+
| saved_eip        | <- Original EIP before signal
+------------------+
| saved_esp        | <- Original ESP before signal
+------------------+
| trampoline[12]   | <- Executable code
+------------------+
| signum           | <- Argument to handler
+------------------+
| &trampoline      | <- Return address (points to trampoline)
+------------------+
Low Address (new ESP)
```

3. **sys_sigreturn()**: Reads saved ESP/EIP from stack and restores trapframe:
```c
void sys_sigreturn(tf_t *tf) {
    unsigned int cur_pid = get_curid();
    uint32_t saved_esp_addr, saved_eip_addr;

    tcb_get_signal_context(cur_pid, &saved_esp_addr, &saved_eip_addr);

    uint32_t saved_esp, saved_eip;
    pt_copyin(cur_pid, saved_esp_addr, &saved_esp, sizeof(uint32_t));
    pt_copyin(cur_pid, saved_eip_addr, &saved_eip, sizeof(uint32_t));

    tf->esp = saved_esp;
    tf->eip = saved_eip;

    tcb_clear_signal_context(cur_pid);
}
```

---

### Bug #7: SIGKILL Hangs Instead of Terminating

**Symptom**: `kill -9 PID` set pending signal but process never terminated (was blocked on IPC).

**Root Cause**: SIGKILL was being set as pending, waiting for process to run and check signals. But blocked processes never run!

**Fix**: Handle SIGKILL immediately in `sys_kill()`:
```c
void sys_kill(tf_t *tf) {
    // ... validation ...

    // SIGKILL is special - terminate immediately
    if (signum == SIGKILL) {
        tcb_set_state(pid, TSTATE_DEAD);
        tqueue_remove(NUM_IDS, pid);
        tcb_set_pending_signals(pid, 0);
        syscall_set_errno(tf, E_SUCC);
        return;
    }

    // Other signals: set as pending
    tcb_add_pending_signal(pid, signum);
    // ...
}
```

---

### Bug #8: thread_yield() Re-queues Terminated Process

**Symptom**: When current process handles SIGKILL to itself, calling `thread_yield()` would put it back in ready queue.

**Root Cause**: `thread_yield()` always sets current process to READY and enqueues it:
```c
void thread_yield(void) {
    old_cur_pid = get_curid();
    tcb_set_state(old_cur_pid, TSTATE_READY);  // Bug: sets DEAD process to READY!
    tqueue_enqueue(NUM_IDS, old_cur_pid);       // Bug: re-queues terminated process!
    // ...
}
```

**Fix**: Added `thread_exit()` that switches without re-queuing:
```c
void thread_exit(void) {
    old_cur_pid = get_curid();
    // Do NOT set state to READY or enqueue

    new_cur_pid = tqueue_dequeue(NUM_IDS);
    tcb_set_state(new_cur_pid, TSTATE_RUN);
    set_curid(new_cur_pid);

    kctx_switch(old_cur_pid, new_cur_pid);
    // Never returns
}
```

---

## Final Working Implementation

### Signal Flow (Complete)

```
1. REGISTRATION (trap N):
   User: sigaction(signum, &sa, NULL)
   └─> Syscall trap
       └─> sys_sigaction()
           └─> pt_copyin(user sigaction → kernel)
           └─> tcb_set_sigaction(pid, signum, &kern_act)

2. SENDING (kill -N PID):
   User: kill(pid, signum)
   └─> Syscall trap
       └─> sys_kill()
           ├─> [If SIGKILL] Immediate termination
           │   ├─> tcb_set_state(pid, TSTATE_DEAD)
           │   ├─> tqueue_remove(NUM_IDS, pid)
           │   └─> Return success
           └─> [Other signals]
               └─> tcb_add_pending_signal(pid, signum)

3. DELIVERY (on trap return):
   trap() function:
   └─> set_pdir_base(0)  // Kernel page table
   └─> handle interrupt/syscall
   └─> handle_pending_signals(tf)  // BEFORE switching to user PT!
       └─> for each pending signal:
           └─> tcb_clear_pending_signal()
           └─> [If SIGKILL] terminate_process() + thread_exit()
           └─> [Other] deliver_signal(tf, signum)
               ├─> Save orig ESP/EIP to user stack via pt_copyout()
               ├─> Write trampoline code to user stack
               ├─> Push signum argument
               ├─> Push trampoline address as return addr
               ├─> tcb_set_signal_context() to save addresses
               ├─> tf->esp = new_esp
               └─> tf->eip = handler_address
   └─> set_pdir_base(cur_pid)  // User page table
   └─> Return to user (executes handler!)

4. HANDLER EXECUTION (user space):
   signal_handler(signum):
   └─> printf("Received signal %d", signum)
   └─> ret  // Returns to trampoline

5. SIGRETURN (trampoline execution):
   Trampoline code:
   └─> mov eax, SYS_sigreturn
   └─> int 0x30
   └─> sys_sigreturn()
       ├─> tcb_get_signal_context() to get saved addresses
       ├─> pt_copyin() to read saved ESP/EIP
       ├─> tf->esp = saved_esp
       ├─> tf->eip = saved_eip
       └─> tcb_clear_signal_context()
   └─> Return to original user code location!
```

### Key Insight: Page Table Ordering

The most critical lesson learned:

```
┌────────────────────────────────────────────────────────────────┐
│  SIGNAL HANDLING MUST OCCUR WITH KERNEL PAGE TABLE ACTIVE!    │
│                                                                 │
│  The kernel page table has identity mapping for physical       │
│  memory (1GB-3.75GB), allowing pt_copyout() to work correctly. │
│                                                                 │
│  The user page table does NOT have this mapping, so kernel     │
│  code reading PTEs would get wrong physical addresses.         │
└────────────────────────────────────────────────────────────────┘
```

---

## Testing Procedures

### Test 1: Basic Signal Handler

```bash
# Register handler for signal 2
trap 2
# Expected: Handler registered successfully.

# Send signal 2 to shell (PID 2)
kill -2 2
# Expected: *** Received signal 2 ***
# Shell continues working
```

### Test 2: Multiple Signals

```bash
trap 2
trap 3
kill -2 2
# Expected: *** Received signal 2 ***
kill -3 2
# Expected: *** Received signal 3 ***
```

### Test 3: SIGKILL Process Termination

```bash
# Spawn a background process
spawn 1
# Expected: Process spawned with PID N

# Kill it
kill -9 N
# Expected: Process N terminated by SIGKILL

# Try to kill again
kill -9 N
# Expected: Failed to send signal (process is DEAD)
```

### Test 4: Error Handling

```bash
kill -0 2       # Invalid signal 0
# Expected: Invalid signal number

kill -99 2      # Invalid signal 99
# Expected: Invalid signal number

kill -2 999     # Invalid PID
# Expected: Invalid PID

kill -5 2       # No handler registered
# Expected: Signal sent, but no handler executes
```

---

## Lessons Learned

### 1. Page Table Context Matters

When kernel code accesses user memory via page table walks, ensure you're using the correct page table. The kernel page table has identity mapping; user page tables don't.

### 2. Stack Layout for cdecl

For x86 cdecl calling convention:
- Return address must be at `[ESP]`
- First argument at `[ESP+4]`
- Push in reverse order: argument first, then return address

### 3. Copy User Data Properly

Never directly dereference user-space pointers. Always use `pt_copyin()` / `pt_copyout()`.

### 4. SIGKILL is Special

SIGKILL must terminate immediately in `sys_kill()`, not wait for the process to be scheduled.

### 5. Process Termination Needs Special Handling

Use `thread_exit()` (not `thread_yield()`) when terminating a process to avoid re-queuing it.

### 6. Debug Output is Essential

Liberal use of `KERN_INFO()` to trace:
- Syscall entry/exit
- Page table operations
- Stack addresses and values
- Signal state changes

### 7. Verify at Every Step

After each fix, verify with actual tests. What appears correct in code may fail in execution.

---

## Quick Reference

### Shell Commands

| Command | Description | Example |
|---------|-------------|---------|
| `trap N` | Register handler for signal N | `trap 2` |
| `kill -N PID` | Send signal N to process PID | `kill -2 2` |
| `spawn ID` | Spawn process (1=ping, 2=pong) | `spawn 1` |

### Signal Numbers

| Signal | Number | Behavior |
|--------|--------|----------|
| SIGKILL | 9 | Immediate termination (cannot catch) |
| Others | 1-31 | Delivered to handler if registered |

### Key Files

- Signal handling: `kern/trap/TTrapHandler/TTrapHandler.c`
- Syscalls: `kern/trap/TSyscall/TSyscall.c`
- TCB state: `kern/thread/PTCBIntro/PTCBIntro.c`
- Thread management: `kern/thread/PThread/PThread.c`
- User wrappers: `user/lib/signal.c`
- Shell commands: `user/shell/shell.c`

---

*Document created after successful implementation - February 2026*
