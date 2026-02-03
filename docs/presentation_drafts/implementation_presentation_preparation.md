# Signal Implementation Presentation — Preparation Guide

This document provides detailed explanations of each slide's concepts and anticipated Q&A.

---

# Part 1: Detailed Concept Explanations

---

## Slides 1-2: Title and Agenda

**What you're setting up:**

You're telling the audience that this presentation is about *how* signals work internally, not just *what* they are (which was covered in the previous presentation). The key message is: we're going from user-perspective to kernel-perspective.

**Framing:**
> "Last time we talked about what signals are and how to use them from a program. Today, we're going to look under the hood — how does the kernel actually make this magic happen? We'll build up the intuition first, understand why it's designed this way, and then see how we implement it in mCertiKOS."

---

## Slides 3-4: Key Terminology

### Trap Frame

When a process transitions from user mode to kernel mode (for any reason — syscall, interrupt, exception), the CPU automatically saves the current state onto the kernel stack. This saved state is called the **trap frame**.

Think of it like a "save game" in a video game. Before the kernel takes over, the CPU "saves" exactly where the user program was:
- **EIP** (Instruction Pointer): Which instruction was about to execute
- **ESP** (Stack Pointer): Where the user's stack was
- **EFLAGS**: Status flags (like whether interrupts were enabled)
- **General registers**: EAX, EBX, ECX, EDX, etc.

**Why it matters for signals:** The trap frame is our "control panel." By modifying the saved EIP before returning, we can make the CPU go somewhere else when it resumes!

### EIP and ESP

- **EIP** = "Where am I executing?" — The address of the next instruction
- **ESP** = "Where is my stack?" — The address of the top of the stack

These two registers define "where" a program is at any moment. Control EIP = control execution flow.

### IRET (Interrupt Return)

This is the CPU instruction that ends kernel mode and returns to user mode. It:
1. Pops the saved state from the trap frame
2. Restores registers
3. Jumps to the saved EIP
4. Switches back to user privilege level

**Critical insight:** IRET doesn't know or care if we modified the trap frame. It just restores whatever values are there. So if we change EIP from `0x40001234` to `0x40005678`, IRET will happily jump to `0x40005678`.

### TCB (Thread Control Block)

This is the kernel's data structure for each process/thread. It stores:
- Process state (READY, RUNNING, SLEEPING, DEAD)
- Saved context for context switching
- **Our addition:** Signal state (handlers, pending signals)

Think of TCB as the kernel's "file" on each process — everything it needs to know.

### Pending Signal

A signal that has been sent but not yet delivered. It's like an unread notification — it exists, but the recipient hasn't seen it yet.

### Trampoline

A small piece of code that acts as a "springboard." In signal handling, the trampoline is code that:
1. Gets executed when the handler returns
2. Calls the `sigreturn` syscall
3. Returns control to the kernel to restore original context

The name comes from the idea of "bouncing" — handler returns → bounce off trampoline → back to kernel → back to original code.

---

## Slides 5-7: Building Intuition

### Slide 5: The Core Problem

**The fundamental challenge:**

Imagine a process is running a loop:
```c
while (1) {
    do_work();
}
```

It's at address `0x40001234`, about to call `do_work()`. Now someone does `kill(pid, SIGINT)`. How do we make this process suddenly run the signal handler at `0x40005678` instead?

**Why this is hard:**
1. The process doesn't know a signal arrived — it's just executing its code
2. We can't just call a function — that would mess up the stack and registers
3. We need to run the handler AND then resume exactly where we left off
4. The signal can arrive at ANY instruction — completely asynchronous

This isn't like a function call where the caller sets things up. This is more like teleportation — we need to yank the process to a different location, let it do something, then put it back exactly where it was.

### Slide 6: Why Kernel Involvement?

**Why can't user space handle this?**

1. **No self-interruption:** A CPU running user code can't interrupt itself. It needs an external event (timer interrupt, I/O, etc.) to cause a transition to kernel.

2. **Asynchronous nature:** The signal might be sent by a completely different process. Process A does `kill(B, SIGINT)`. Process B has no idea this happened — it's just running its code. Only the kernel can coordinate between processes.

3. **Context preservation:** To resume correctly, we need to save ALL CPU state perfectly. User code can't access all CPU registers directly, and can't save/restore the instruction pointer.

4. **Security:** If user processes could directly modify each other's execution, it would be a massive security hole. The kernel acts as a trusted intermediary.

### Slide 7: The Key Insight

**The kernel's opportunity:**

The kernel gets control at predictable moments:
- **System calls:** Process asks kernel to do something (`read()`, `write()`, `sleep()`)
- **Interrupts:** Timer tick, keyboard press, network packet
- **Exceptions:** Page fault, division by zero, invalid instruction

At these moments, the kernel has:
- Full CPU control
- Access to the trap frame (saved user state)
- Ability to modify that state before returning

**The return path is key:** Every kernel entry has a corresponding exit (return to user). At that exit point, the kernel can check "does this process have pending signals?" and act accordingly.

This is why signals feel asynchronous to the user program — they're delivered at the next kernel exit, which happens frequently (timer interrupts occur ~100-1000 times per second).

---

## Slide 8: The Trap Frame

**Deep dive into the trap frame:**

When you execute `int 0x30` (syscall) or a timer interrupt fires, the CPU does this automatically:

```
1. Switch to kernel stack (from TSS)
2. Push SS (user stack segment)
3. Push ESP (user stack pointer)
4. Push EFLAGS
5. Push CS (user code segment)
6. Push EIP (where to resume)
7. Jump to interrupt handler
```

Then the kernel's handler code pushes more:
```
8. Push error code (or 0)
9. Push trap number
10. Push all general registers (PUSHA)
11. Push segment registers (DS, ES)
```

This complete structure is the trap frame. In code:

```c
struct tf_t {
    // Pushed by software
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint16_t es, ds;
    uint32_t trapno, err;
    // Pushed by CPU
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;  // Only if privilege change
};
```

**The magic:** `tf->eip` contains where the process will resume. Change it = change destination!

---

## Slides 9-12: Signal Lifecycle

### Slide 9: Overview

The signal lifecycle has distinct phases. Understanding each phase helps understand where each piece of code lives.

### Slide 10: Generation and Pending

**The bitmask approach:**

We use a 32-bit integer where each bit represents a signal:
- Bit 1 = SIGHUP
- Bit 2 = SIGINT
- Bit 9 = SIGKILL
- etc.

**Why bitmask?**
1. **Space efficient:** 32 signals in 4 bytes
2. **Fast operations:** Setting/clearing/checking is O(1)
3. **Natural coalescing:** Multiple same signals = still one bit

**Limitation:** If you send SIGUSR1 three times, only one is recorded. For most signals this is fine — you just need to know "a signal arrived," not "how many."

**Operations:**
```c
// Send signal 2 (SIGINT)
pending_signals |= (1 << 2);     // Set bit 2

// Clear signal 2 after delivery
pending_signals &= ~(1 << 2);    // Clear bit 2

// Check if signal 2 is pending
if (pending_signals & (1 << 2)) { ... }

// Find any pending signal
for (int sig = 1; sig < 32; sig++) {
    if (pending_signals & (1 << sig)) {
        // Found one!
    }
}
```

### Slide 11: The Delivery Moment

**When exactly do we deliver?**

Answer: At **every** return from kernel to user mode.

This includes:
- Returning from a syscall
- Returning from a timer interrupt
- Returning from a page fault handler
- Returning after context switch

Why here?
1. **Trap frame is ready:** We have access to the saved state
2. **Clean transition point:** We're about to go back anyway
3. **Happens frequently:** Timer interrupts ensure we check often (even if process never makes syscalls)

**The check:**
```c
void trap_return(tf_t *tf) {
    // Check for pending signals
    if (tcb->pending_signals != 0) {
        // Find highest priority unblocked signal
        for (int sig = 1; sig < 32; sig++) {
            if (pending_signals & (1 << sig)) {
                deliver_signal(tf, sig);
                break;  // One at a time
            }
        }
    }
    // Now do the actual return (IRET)
}
```

### Slide 12: The EIP Hijack

**This is the core trick of the entire mechanism.**

Before delivery:
```
tf->eip = 0x40001234  (middle of user's main() function)
```

After delivery:
```
tf->eip = 0x40005678  (signal handler function)
```

When IRET executes, CPU loads EIP from trap frame → jumps to handler!

**It's that simple conceptually.** One assignment. But making it work correctly requires handling the return path (slides 13-15).

---

## Slides 13-15: The Return Problem and Solution

### Slide 13: The Problem

When a normal function is called:
```asm
; Caller does:
push arg1        ; Push arguments
call function    ; Push return address, jump to function

; Callee does:
push ebp         ; Save old frame pointer
mov ebp, esp     ; Setup new frame
; ... do work ...
pop ebp          ; Restore frame pointer
ret              ; Pop return address, jump there
```

The key is `call` pushes the return address. When `ret` executes, it pops that address and jumps there.

**With signals, we didn't use `call`.** We just changed EIP directly. There's no return address on the stack! When the handler does `ret`, it will pop garbage and jump to a random address → crash.

### Slide 14: The Trampoline Solution

**We need to set up a return path manually.**

The idea:
1. Before jumping to handler, put a "return address" on the stack
2. This address points to special code (the trampoline)
3. Trampoline calls `sigreturn` syscall
4. Kernel restores original context

**Why sigreturn?**
The kernel saved the original EIP/ESP. Only the kernel can restore them properly. The trampoline is just a way to get back into the kernel with the right syscall.

**The trampoline code:**
```asm
mov eax, SYS_sigreturn   ; Syscall number
int 0x30                  ; Trap to kernel
jmp $                     ; Infinite loop (never reached)
```

This is just 9 bytes! We write these bytes directly onto the user stack.

### Slide 15: Stack Layout

**What we set up before jumping to handler:**

```
Starting ESP: 0x7FFFFF00

After setup:
                     Address       Content
                     ─────────────────────────────────
                     0x7FFFFF00    (original stack data)
                     0x7FFFFFEC    saved original EIP  ← for sigreturn
                     0x7FFFFFE8    saved original ESP  ← for sigreturn
                     0x7FFFFFE0    trampoline code (9 bytes)
                     0x7FFFFFDC    signal number (e.g., 2)  ← handler arg
ESP after setup →    0x7FFFFFD8    trampoline address      ← return addr
```

When handler starts:
- `[ESP]` = return address (points to trampoline)
- `[ESP+4]` = signal number (first argument per cdecl)

When handler does `ret`:
- Pops return address = trampoline address
- Jumps to trampoline
- Trampoline does `int 0x30` with `eax = SYS_sigreturn`
- Kernel's `sys_sigreturn` reads saved EIP/ESP from stack
- Kernel restores trap frame with original values
- IRET returns to original code location

**Beautiful full circle!**

---

## Slide 16: Complete Signal Flow

This slide ties everything together in a sequence diagram. Walk through it step by step:

1. Process running normally
2. Some trap occurs (syscall, timer, etc.)
3. Kernel checks pending signals — yes, there's one!
4. Kernel sets up stack: save context, write trampoline, push args
5. Kernel modifies trap frame: EIP = handler, ESP = adjusted
6. IRET → CPU jumps to handler
7. Handler executes, does its thing
8. Handler returns → pops return address → jumps to trampoline
9. Trampoline executes `int 0x30` with SYS_sigreturn
10. Kernel's sys_sigreturn restores original EIP/ESP to trap frame
11. IRET → CPU jumps to original location
12. Process continues as if nothing happened (except handler ran)

---

## Slides 17-19: Applying to mCertiKOS

### Slide 17: What We Need

**Data structures:**
- Each TCB needs signal state
- Signal state includes: array of handlers, pending bitmask, saved context

**Syscalls:**
- `sigaction(signum, action)` — Register handler
- `kill(pid, signum)` — Send signal
- `pause()` — Wait for any signal
- `sigreturn()` — Restore context after handler

**Delivery logic:**
- Hook into trap return path
- Check pending signals
- Call deliver_signal if needed

### Slide 18: Data Structures

```c
struct sig_state {
    struct sigaction sigactions[32];  // One per signal
    uint32_t pending_signals;          // Bitmask
    uint32_t saved_esp;                // For sigreturn
    uint32_t saved_eip;                // For sigreturn
    int in_handler;                    // Prevent nesting issues
};
```

This goes inside each TCB. When a process is created, initialize:
- All handlers to NULL (default action)
- pending_signals to 0
- in_handler to 0

### Slide 19: Implementation Steps

| Step | What | Where | Notes |
|------|------|-------|-------|
| 1 | Add sig_state to TCB | PTCBIntro.c | Just struct addition |
| 2 | sys_sigaction | TSyscall.c | Copy handler from user space |
| 3 | sys_kill | TSyscall.c | Set bit in target's pending |
| 4 | Delivery check | TTrapHandler.c | Call before IRET |
| 5 | deliver_signal | TTrapHandler.c | Setup stack + modify tf |
| 6 | Trampoline | (inline) | Write bytes to stack |
| 7 | sys_sigreturn | TSyscall.c | Restore saved context |

---

## Slides 20-21: Critical Gotchas

### Slide 20: Page Table Timing

**This is the bug that took the longest to find in our implementation.**

**The setup:**
- Kernel has its own page table (PID 0)
- Each user process has its own page table
- In mCertiKOS kernel page table has "identity mapping" for physical memory

**Identity mapping means:** Physical address 0x12345 = Virtual address 0x12345

**The problem:**
```c
// In trap handler
set_pdir_base(cur_pid);        // Switch to USER's page table
handle_pending_signals(tf);     // Try to write to user stack
```

When we call `pt_copyout()` to write to user stack, it:
1. Takes a physical address (from kernel's view)
2. Writes to that physical address

But we already switched to user's page table! User's page table doesn't have identity mapping. The physical address we're trying to write to doesn't map to the same virtual address in user space. Result: we write to wrong memory or page fault.

**The fix:**
```c
// CORRECT order
handle_pending_signals(tf);     // Write while still in kernel PT
set_pdir_base(cur_pid);         // THEN switch to user PT
```

**Lesson:** Page table context matters. Always think about "which page table is active when I access memory?"

### Slide 21: SIGKILL Special Case

**SIGKILL (signal 9) is special by design:**
- Cannot be caught (no handler allowed)
- Cannot be ignored
- Cannot be blocked
- Must terminate the process

**The implementation issue:**

Normal signals: Set pending bit → wait for process to enter kernel → deliver at return

But what if the process is blocked? For example:
```c
sleep(1000000);  // Process is sleeping, won't return soon
```

If we just set the pending bit, the process won't see the signal until it wakes up!

**Solution:** Handle SIGKILL immediately in `sys_kill`:
```c
void sys_kill(tf_t *tf) {
    int pid = get_arg(tf, 1);
    int signum = get_arg(tf, 2);

    if (signum == SIGKILL) {
        // Don't set pending, just kill immediately
        tcb_set_state(pid, TSTATE_DEAD);
        tqueue_remove(NUM_IDS, pid);  // Remove from scheduler
        return;
    }

    // Normal signals: set pending
    tcb->pending_signals |= (1 << signum);
}
```

---

# Part 2: Anticipated Questions & Answers

---

## Q1: Why check signals at trap return and not immediately when kill() is called?

**Answer:**

Several reasons:

1. **The target process might be running on a different CPU.** In a multi-core system, process A (running on CPU 0) calls kill() targeting process B (running on CPU 1). A's kernel can't directly interrupt B's CPU. It can only set a flag that B will check.

2. **The target might be in the middle of a critical operation.** If we interrupted immediately, we could corrupt state. The trap return is a safe point — the process is in a clean state.

3. **Simplicity.** Checking at one well-defined point (trap return) is much simpler than trying to interrupt a process mid-execution.

4. **Happens soon anyway.** Timer interrupts occur frequently (100-1000 Hz), so even a process in an infinite loop will hit trap return within ~1-10ms.

---

## Q2: What if multiple signals are pending at the same time?

**Answer:**

We deliver them **one at a time**, in order of signal number (lowest first).

```c
for (int sig = 1; sig < 32; sig++) {
    if (pending & (1 << sig)) {
        deliver_signal(tf, sig);
        break;  // Only one!
    }
}
```

Why one at a time?
- Simpler stack management
- Each handler gets clean context
- Next trap return will deliver the next one

So if SIGINT (2) and SIGUSR1 (10) are both pending:
1. First trap return: deliver SIGINT
2. Handler runs, returns via sigreturn
3. Second trap return: deliver SIGUSR1
4. Handler runs, returns via sigreturn
5. Resume original code

---

## Q3: Can a signal handler be interrupted by another signal?

**Answer:**

In our simplified implementation: **No.**

We set `in_handler = 1` during handler execution and don't deliver new signals until it's done.

In full POSIX: **Yes, by default.** But you can control this with `sa_mask` in sigaction — specify which signals to block during handler execution.

Nested signals get complicated (need to save multiple contexts), so we avoided it.

---

## Q4: Why write trampoline code to the stack? Isn't that a security risk?

**Answer:**

**Yes, in modern systems it is!** This is called "executable stack" and is disabled by default on modern Linux (NX bit, W^X policy).

**For mCertiKOS:** It's an educational OS, so we use this simpler approach.

**Real Linux solution:** The trampoline code lives in a special kernel-mapped page called the "vDSO" (virtual Dynamic Shared Object) that's mapped into every process at a known address. The kernel puts the return address pointing to this shared trampoline, not code on the stack.

Alternative in some systems: Use `sa_restorer` field in sigaction — user provides their own trampoline function.

---

## Q5: What happens if the signal handler itself causes a signal (like SIGSEGV)?

**Answer:**

This can lead to infinite recursion or crash:

1. Handler runs
2. Handler accesses bad memory → SIGSEGV
3. Kernel tries to deliver SIGSEGV → calls handler
4. Handler runs again
5. Same bad access → SIGSEGV
6. Repeat forever (or until stack overflow)

**Solutions:**
1. **Block the same signal during handler** (SA_NODEFER flag controls this)
2. **Reset handler to default after delivery** (SA_RESETHAND flag)
3. **Kernel detects recursion and terminates** (what Linux does for fatal signals)

In our implementation, `in_handler` flag prevents re-delivery, so step 3 would be blocked until first handler returns.

---

## Q6: How does sigreturn know where to restore from?

**Answer:**

We save the original EIP and ESP at known locations relative to the handler's stack:

```c
// In deliver_signal:
user_esp -= 4;
pt_copyout(&tf->eip, pid, user_esp, 4);  // Save EIP
saved_eip_addr = user_esp;                // Remember where

user_esp -= 4;
pt_copyout(&tf->esp, pid, user_esp, 4);  // Save ESP
saved_esp_addr = user_esp;                // Remember where
```

We store `saved_eip_addr` and `saved_esp_addr` in the TCB's sig_state.

In sys_sigreturn:
```c
void sys_sigreturn(tf_t *tf) {
    uint32_t saved_eip, saved_esp;
    pt_copyin(pid, tcb->sigstate.saved_eip_addr, &saved_eip, 4);
    pt_copyin(pid, tcb->sigstate.saved_esp_addr, &saved_esp, 4);

    tf->eip = saved_eip;
    tf->esp = saved_esp;
    // IRET will now return to original location
}
```

---

## Q7: Why use pt_copyout/pt_copyin instead of direct memory access?

**Answer:**

**User space memory isn't directly accessible from kernel in a safe way.**

1. **Different address spaces:** The user's address 0x7FFFFF00 means nothing in kernel's address space. We need to translate through page tables.

2. **Validation:** `pt_copyout` checks that the address is valid and writable for that process. Direct access might hit unmapped memory.

3. **Page table walking:** These functions walk the process's page table to find the physical address, then write to that physical address.

In mCertiKOS specifically, `pt_copyout(data, pid, user_addr, size)`:
1. Looks up `user_addr` in process `pid`'s page table
2. Finds the physical page
3. Writes `data` to that physical location (using kernel's identity mapping)

---

## Q8: What's the difference between signals, interrupts, and traps?

**Answer:**

| Aspect | Interrupt | Trap | Signal |
|--------|-----------|------|--------|
| **Source** | Hardware (timer, keyboard, disk) | Software (CPU instruction or exception) | Software (kernel, other processes) |
| **Trigger** | External hardware event | `int` instruction, division by zero, page fault | `kill()` syscall, kernel event |
| **Target** | CPU/Kernel | Kernel | Specific process |
| **Handler** | Kernel code only | Kernel code only | User code (mostly) |
| **Synchronous?** | Asynchronous (unpredictable timing) | Synchronous (caused by current instruction) | Asynchronous (delivered at trap return) |
| **Execution** | Immediate, preempts current code | Immediate, part of instruction execution | Deferred to trap return |
| **Context** | Kernel context | Kernel context | User context (handler runs as user) |
| **Example** | Timer tick, keyboard press | `int 0x30` (syscall), divide by zero | SIGINT, SIGKILL, SIGSEGV |

**Key Distinctions:**

- **Interrupt vs Trap:** Interrupts come from *outside* the CPU (hardware). Traps come from *inside* (the currently executing instruction). Both enter the kernel.

- **Trap vs Signal:** Traps are the *mechanism* to enter kernel. Signals are a *notification* to a process. A trap can *cause* a signal (e.g., divide-by-zero trap → SIGFPE signal).

**Relationship — How they connect:**

```
Hardware Event          Trap (CPU Exception)         Software Request
      │                        │                            │
      ▼                        ▼                            ▼
  Interrupt               Exception                    kill() syscall
      │                        │                            │
      └────────────────────────┼────────────────────────────┘
                               ▼
                      Kernel gets control
                               │
                               ▼
                    (May generate a signal)
                               │
                               ▼
                      Signal set pending
                               │
                               ▼
                   At trap return → deliver
```

**Examples of the chain:**
- Timer interrupt → kernel checks if alarm expired → sends SIGALRM
- Keyboard Ctrl+C → keyboard interrupt → kernel sends SIGINT to foreground process
- Page fault (trap) on bad address → kernel sends SIGSEGV
- `int 0x30` (trap) for kill syscall → kernel sends signal to target

---

## Q9: What if a process ignores all signals?

**Answer:**

Most signals can be ignored by setting handler to SIG_IGN:
```c
signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
```

**But SIGKILL and SIGSTOP cannot be ignored.** This is by design — the system administrator must always have a way to stop a runaway process.

If a process ignores all other signals, you can still:
- `kill -9 pid` (SIGKILL)
- `kill -STOP pid` (SIGSTOP) to pause it
- `kill -CONT pid` (SIGCONT) to resume

---

## Q10: How do real operating systems (Linux) handle signals differently?

**Answer:**

Linux has more sophisticated handling:

1. **Signal queuing for real-time signals:** SIGRTMIN to SIGRTMAX can queue multiple instances with data payloads.

2. **siginfo_t:** Extended handler signature `void handler(int, siginfo_t*, void*)` provides sender PID, user ID, fault address, etc.

3. **Alternate signal stack:** `sigaltstack()` lets you designate a separate stack for handlers — useful for handling stack overflow!

4. **vDSO trampoline:** Trampoline code is in a shared kernel-mapped page, not on user stack (security).

5. **Robust sigreturn:** Linux saves/restores the entire register set, floating-point state, and more.

6. **Signal masks with threads:** Each thread has its own signal mask; some signals can be directed to specific threads.

Our implementation is simplified but captures the core concepts.

---

## Q11: What is the overhead of signal handling?

**Answer:**

Signal handling is relatively expensive:

1. **At least 2 kernel transitions:** Trap in (for delivery check), trap out to handler, trap in (sigreturn), trap out to resume.

2. **Stack manipulation:** Writing trampoline, saving context, restoring context.

3. **TLB/cache effects:** Jumping between user code and handler may cause cache misses.

**Rough numbers (modern Linux):**
- Simple signal delivery: ~1-5 microseconds
- Compare to function call: ~1-10 nanoseconds

This is why signals aren't used for high-frequency communication. For that, use shared memory, pipes, or sockets.

---

## Q12: Why is the calling convention (cdecl) important here?

**Answer:**

When the signal handler is a C function:
```c
void handler(int signum) { ... }
```

The compiler generates code expecting arguments at specific locations:
- cdecl (x86 32-bit): First arg at `[ESP+4]` (after return address at `[ESP]`)
- After function prologue: First arg at `[EBP+8]`

**If we don't set up the stack correctly:**
```c
void handler(int signum) {
    printf("Got signal %d\n", signum);  // Would print garbage!
}
```

The handler would read garbage from the wrong stack location.

**Our stack setup follows cdecl:**
```
[ESP]   = return address (trampoline)
[ESP+4] = signum
```

This matches what the compiler expects, so `signum` parameter works correctly.

---

## Q13: Can a process send signals to any other process?

**Answer:**

**No, there are permission checks:**

1. **Same user:** A process can signal other processes owned by the same user.

2. **Root/superuser:** Can signal any process.

3. **Same process:** A process can always signal itself.

In our mCertiKOS implementation, we simplified this — any process can signal any other (no permission checks). A real implementation would check:
```c
if (sender_uid != 0 && sender_uid != target_uid) {
    return -EPERM;  // Permission denied
}
```

---

## Q14: What happens if the handler never returns?

**Answer:**

The process stays "in the handler" forever:

```c
void handler(int signum) {
    while (1) { }  // Never returns
}
```

Effects:
- Original code never resumes
- sigreturn never called
- Process is effectively stuck in handler

This isn't necessarily a bug — some handlers are designed to not return:
```c
void handler(int signum) {
    cleanup();
    exit(0);  // Terminate instead of returning
}
```

In our implementation, if `in_handler = 1` forever, no more signals would be delivered to this process.

---

## Q15: Why save EIP and ESP specifically? Why not the whole trap frame?

**Answer:**

**Trade-off: Simplicity vs Completeness**

**Saving only EIP/ESP (our approach):**
- Pros: Simple, small stack footprint
- Cons: Other registers might be clobbered by handler

**Saving entire trap frame (Linux approach):**
- Pros: Perfect restoration, handler can use all registers
- Cons: More complex, larger stack footprint (~60+ bytes)

For our educational implementation, EIP/ESP is enough because:
1. The handler is a C function that follows calling conventions
2. Callee-saved registers (EBX, ESI, EDI, EBP) are preserved by the handler
3. Caller-saved registers (EAX, ECX, EDX) are considered clobberable anyway

For production systems, you'd save everything including floating-point state!

---

# Part 3: Quick Reference

## Key Formulas

```c
// Set signal pending
pending |= (1 << signum);

// Clear signal pending
pending &= ~(1 << signum);

// Check if pending
if (pending & (1 << signum)) { ... }

// Find any pending signal
for (sig = 1; sig < 32; sig++)
    if (pending & (1 << sig)) break;
```

## Key Addresses (typical)

| Item | Address Range |
|------|---------------|
| User code | 0x40000000 - 0x7FFFFFFF |
| User stack | Near 0x7FFFFFFF (grows down) |
| Kernel | 0xC0000000+ |

## Trampoline Bytes

```
B8 42 00 00 00    mov eax, 66 (SYS_sigreturn)
CD 30             int 0x30
EB FE             jmp $ (infinite loop)
```

## cdecl Stack Layout

```
[ESP]     = return address
[ESP+4]   = 1st argument
[ESP+8]   = 2nd argument
...
[EBP]     = saved EBP (after prologue)
[EBP+4]   = return address
[EBP+8]   = 1st argument
```
