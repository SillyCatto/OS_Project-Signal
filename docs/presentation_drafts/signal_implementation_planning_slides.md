# Planning & Roadmap for Implementing POSIX-Compliant Signal Mechanism on mCertiKOS

> Presentation Slides

---

## Slide 1: Title

### Planning & Roadmap for Implementing POSIX-Compliant Signal Mechanism on mCertiKOS

- **Context:** mCertiKOS — educational x86 OS with layered verification architecture
- **Goal:** Add POSIX-style signal handling (registration, delivery, return) to the existing kernel


---

## Slide 2: What Are We Building?

**A complete signal pipeline — from user `kill()` to handler execution and seamless return**

- **4 new system calls:** `sigaction`, `kill`, `pause`, `sigreturn`
- **Signal delivery engine:** intercepts trap return path, hijacks EIP to redirect execution
- **Inline trampoline:** raw x86 machine code on user stack triggers automatic `sigreturn`
- **User-space API:** POSIX-compatible `sigaction()`, `kill()`, `pause()` wrappers
- **Shell commands:** interactive `kill`, `trap`, `spawn` for testing

**What we are NOT building:**
- Real-time signals, signal queuing, `SA_SIGINFO` extended handlers
- Multi-CPU signal targeting
- Full signal blocking/masking enforcement

---

## Slide 3: Architecture — The 5-Layer View

```mermaid
graph TB
    subgraph "User Space (Ring 3)"
        A["user/lib/signal.c<br/>sigaction(), kill(), pause()"]
        B["user/include/signal.h<br/>struct sigaction, constants"]
        C["user/include/syscall.h<br/>sys_sigaction(), sys_kill()<br/>inline asm wrappers"]
        D["user/shell/shell.c<br/>kill, trap, spawn commands"]
        E["Signal Handler<br/>void handler(int signum)"]
    end

    subgraph "Kernel Trap Layer (Ring 0)"
        F["TDispatch.c<br/>Routes syscall numbers to handlers"]
        G["TSyscall.c<br/>sys_sigaction, sys_kill,<br/>sys_pause, sys_sigreturn"]
        H["TTrapHandler.c<br/>deliver_signal,<br/>handle_pending_signals,<br/>trap()"]
    end

    subgraph "Kernel Thread Layer (Ring 0)"
        I["PTCBIntro.c<br/>TCB + signal accessors"]
        J["PThread.c<br/>thread_yield, thread_exit"]
    end

    subgraph "Kernel Data (Ring 0)"
        K["signal.h<br/>struct sig_state,<br/>struct sigaction"]
        L["syscall.h<br/>SYS_sigaction, SYS_kill, ..."]
        M["thread.h<br/>struct thread"]
    end

    A --> C
    C -->|"int 0x30"| F
    F --> G
    G --> I
    H --> I
    H --> J
    D --> A
    E -.->|"called by deliver_signal<br/>via EIP hijack"| H

    style H fill:#ff9999,stroke:#cc0000
    style E fill:#99ff99,stroke:#00cc00
```

- CertiKOS enforces **layered isolation** — each module only accesses others via `export.h`/`import.h`
- TCB accessors are the *only* way to read/write signal state from other layers

---

## Slide 4: Data Structures — `struct sigaction`

**Per-signal configuration (20 bytes, mirrors POSIX)**

| Field | Size | Purpose |
|-------|------|---------|
| `sa_handler` | 4B | Pointer to handler function (`void (*)(int)`) |
| `sa_sigaction` | 4B | Extended 3-arg handler (defined, unused) |
| `sa_flags` | 4B | Behavior flags — `SA_RESTART`, `SA_NODEFER`, etc. |
| `sa_restorer` | 4B | Custom restorer (defined, unused) |
| `sa_mask` | 4B | Bitmask of signals to block during handler |

- **Why mirror POSIX?** User code looks identical to standard Linux programs
- **Only `sa_handler` is functionally required** for basic implementation
- Entire struct is copied as a 20-byte block via `pt_copyin`/`pt_copyout` — no per-field marshalling

---

## Slide 5: Data Structures — `struct sig_state` & TCB Integration

**Per-process signal state (656 bytes, embedded in TCB)**

| Field | Size | Purpose |
|-------|------|---------|
| `sigactions[32]` | 640B | One `sigaction` per signal number |
| `pending_signals` | 4B | Bitmask — bit N = signal N is pending |
| `signal_block_mask` | 4B | Bitmask of blocked signals |
| `saved_esp_addr` | 4B | User stack address where original ESP was saved |
| `saved_eip_addr` | 4B | User stack address where original EIP was saved |
| `in_signal_handler` | 4B | Re-entrancy guard flag |

```mermaid
graph LR
    subgraph "TCBPool[pid]"
        A[state: t_state]
        B[prev: uint]
        C[next: uint]
        D[channel: void*]
        E[openfiles: file*]
        F[cwd: inode*]
        G["sigstate: struct sig_state<br/>(656 bytes)"]
    end

    G --> H["sigactions[0..31]"]
    G --> I["pending_signals"]
    G --> J["saved_esp_addr"]
    G --> K["saved_eip_addr"]
    G --> L["in_signal_handler"]

    style G fill:#ffcc99
```

- **Pending bitmask:** `|= (1 << signum)` to set, `&= ~(1 << signum)` to clear
- **Saved addresses (not values):** `sigreturn` reads the original ESP/EIP from the user stack via `pt_copyin`

---

## Slide 6: Syscall Plumbing — How User Code Reaches the Kernel

```mermaid
graph LR
    A["int 0x30<br/>(user space)"] -->|"hardware<br/>ring switch"| B["idt.S<br/>alltraps:"]
    B -->|"push regs<br/>call trap"| C["TTrapHandler.c<br/>trap()"]
    C -->|"lookup<br/>TRAP_HANDLER"| D["TDispatch.c<br/>syscall_dispatch()"]
    D -->|"switch on<br/>EAX value"| E["TSyscall.c<br/>sys_sigaction()"]
    E -->|"accessor<br/>functions"| F["PTCBIntro.c<br/>tcb_set_sigaction()"]
```

**CertiKOS syscall calling convention:**
- `EAX` = syscall number, `EBX` = arg1, `ECX` = arg2, `EDX` = arg3
- Return value in `EAX` (error code: `E_SUCC` = 0)
- Kernel reads via `syscall_get_arg2(tf)` → tf→regs.ebx (note: arg1 = EAX = syscall number)

**Adding a new syscall requires changes at 3 levels:**
1. `kern/lib/syscall.h` — add enum entry (`SYS_sigaction`, etc.)
2. `TDispatch.c` — add `case SYS_sigaction: sys_sigaction(tf); break;`
3. `TSyscall.c` — implement the handler function

---

## Slide 7: The User-Space Inline Assembly

**Each syscall wrapper is an inline asm block that loads registers and fires `int 0x30`**

```c
asm volatile ("int %1"
      : "=a" (errno)           // OUTPUT: EAX → errno
      : "i" (T_SYSCALL),       // 48 = int 0x30
        "a" (SYS_sigaction),   // EAX ← syscall number
        "b" (signum),          // EBX ← first argument
        "c" (act),             // ECX ← user pointer to new sigaction
        "d" (oldact)           // EDX ← user pointer to old sigaction
      : "cc", "memory");
```

**Key details:**
- `"=a"` — output constraint binds EAX to `errno` after the interrupt returns
- `"i"` — immediate operand (the interrupt vector number, compile-time constant)
- `"cc", "memory"` — tells GCC: flags and memory may change (prevents unsafe optimizations)
- The kernel **cannot dereference** `act`/`oldact` directly — must use `pt_copyin`/`pt_copyout`

---

## Slide 8: Signal Registration — `sys_sigaction`

**Flow: user fills `struct sigaction` → syscall copies it into kernel TCB**

- **Why `pt_copyin`, not `*user_act`?**
  - Kernel runs under kernel page table (identity-mapped: VA = PA)
  - User pointers are virtual addresses valid only under the *user's* page table
  - Direct dereference accesses wrong physical address → crash or garbage

```mermaid
graph LR
    subgraph "User Page Table (pid N)"
        U["VA 0x40001234"] -->|"page table<br/>walk"| P["PA 0x00123234"]
    end

    subgraph "Kernel Page Table (pid 0)"
        K["VA = PA<br/>(identity mapped)"]
    end

    A["pt_copyin(pid, 0x40001234, dst, 20)"]
    A -->|"1. Walk user PT"| U
    A -->|"2. Read from PA"| P
    A -->|"3. Copy to dst"| D["kern_act<br/>(kernel variable)"]
```

- `pt_copyin` — user→kernel (reads user's `sigaction` into kernel variable)
- `pt_copyout` — kernel→user (writes old handler back if `oldact` is provided)

---

## Slide 9: Signal Generation — `sys_kill`

**Flow: sender calls `kill(pid, signum)` → kernel sets pending bit on target TCB**

- **Normal signals (not SIGKILL):**
  - Validate `signum` (1–31) and `pid` (alive process)
  - `tcb_add_pending_signal(pid, signum)` — sets bit in 32-bit bitmask
  - If target is sleeping: call `thread_wakeup()` to move it to ready queue

- **SIGKILL (signal 9) — immediate termination:**
  - Cannot be caught, blocked, or ignored
  - `tcb_set_state(pid, TSTATE_DEAD)` + `tqueue_remove(NUM_IDS, pid)` immediately
  - No pending bit — target is dead right now

- **Why wake sleeping processes?**
  - A sleeping process never reaches the trap return path
  - If not woken, the signal remains pending forever
  - Waking moves it to the ready queue → eventually enters `trap()` → signal delivery fires

---

## Slide 10: The Critical Delivery Point — `trap()` Function

**Signal delivery MUST happen at a specific point in the trap return path**

```mermaid
graph TD
    A["set_pdir_base(0)<br/>Kernel PT active<br/>Identity mapping: VA = PA"] --> B["Handle trap<br/>(syscall, interrupt)"]
    B --> C["kstack_switch(cur_pid)"]
    C --> D["handle_pending_signals(tf)<br/>📌 MUST BE HERE"]
    D --> E["set_pdir_base(cur_pid)<br/>User PT active<br/>No identity mapping"]
    E --> F["trap_return(tf)<br/>IRET → user mode"]

    style D fill:#ff9999,stroke:#cc0000,stroke-width:3px
    style A fill:#ccffcc
    style E fill:#ffcccc
```

- **Why before `set_pdir_base(cur_pid)`?**
  - `deliver_signal()` calls `pt_copyout()` to write trampoline onto user stack
  - `pt_copyout()` needs identity-mapped physical memory access (kernel PT)
  - After switching to user PT: kernel loses physical address access → **crash**

- **This is the #1 implementation pitfall** — placing the call after `set_pdir_base` causes a kernel page fault with no useful error message

---

## Slide 11: Signal Delivery — The Context Hijack (`deliver_signal`)

**The core trick: modify `tf->eip` so that IRET jumps to the handler instead of the original code**

**User stack layout built by `deliver_signal()`:**

```
       High addresses (original ESP)
       ┌──────────────────────┐
       │   original stack     │
       ├──────────────────────┤ ← original tf->esp
       │   saved_eip (4B)     │   ← sigreturn reads this
       ├──────────────────────┤
       │   saved_esp (4B)     │   ← sigreturn reads this
       ├──────────────────────┤
       │   trampoline (12B)   │   ← executable machine code
       ├──────────────────────┤
       │   signum (4B)        │   ← handler arg [ESP+4]  (cdecl)
       ├──────────────────────┤
       │   ret addr (4B)      │   ← → trampoline [ESP+0] (cdecl)
       ├──────────────────────┤ ← new tf->esp
       Low addresses
```

- Everything is written via `pt_copyout()` — kernel writes into user's address space
- **cdecl convention:** `[ESP]` = return address, `[ESP+4]` = first argument
  - Signal number is on the **stack**, NOT in EAX (common mistake)
- After setup: `tf->eip = handler_address`, `tf->esp = new_esp`
- Saved context addresses stored in TCB for `sigreturn` to retrieve later

---

## Slide 12: The Trampoline — Inline x86 Machine Code

**When the handler `ret`urns, execution lands on our trampoline code — which triggers `sigreturn`**

```
Bytes       Assembly               Purpose
──────────────────────────────────────────────────────────
B8 XX 00    mov eax, SYS_sigreturn  Load syscall number into EAX
00 00
CD 30       int 0x30                Trap into kernel → sys_sigreturn
EB FE       jmp $                   Safety: infinite loop if sigreturn fails
90 90 90    nop; nop; nop           Padding to 12 bytes (alignment)
```

- **Why inline machine code?**
  - Self-contained — no dependency on user-space libraries or fixed addresses
  - Written to user stack by kernel via `pt_copyout`
  - Alternative approaches (VDSO, fixed-address page) are more complex

- **`EB FE` = `jmp $` (jump to self):**
  - Relative jump, operand = -2 → jumps back to start of this instruction
  - Safety net: if `sigreturn` fails, process spins instead of executing stack garbage

- **Stack executability:** CertiKOS does not set NX bit → stack is executable

---

## Slide 13: Signal Return — `sys_sigreturn`

**Restores the original execution context so the process resumes where it was interrupted**

```mermaid
graph LR
    A["Handler executes<br/>ret instruction"] --> B["CPU pops [ESP]<br/>→ trampoline_addr"]
    B --> C["Execute trampoline<br/>mov eax, SYS_sigreturn<br/>int 0x30"]
    C --> D["Kernel: sys_sigreturn"]
    D --> E["Read saved ESP & EIP<br/>from user stack"]
    E --> F["Restore tf->esp, tf->eip"]
    F --> G["IRET → original<br/>user code"]
```

**Steps inside `sys_sigreturn()`:**
1. Read `saved_esp_addr` and `saved_eip_addr` from TCB (set during delivery)
2. `pt_copyin()` from those user-stack addresses to get original ESP and EIP values
3. Clear signal context in TCB (`in_signal_handler = 0`)
4. Write restored values into trap frame: `tf->esp = orig_esp`, `tf->eip = orig_eip`
5. When `trap_return()` executes IRET → process resumes at original code, original stack

**Result:** The process has no idea a signal handler ran — completely transparent redirection

---

## Slide 14: Process Termination — SIGKILL & `thread_exit()`

**Why can't we use `thread_yield()` for killed processes?**

| | `thread_yield()` | `thread_exit()` |
|---|---|---|
| Sets state to READY | Yes | **No** |
| Re-enqueues in ready queue | Yes | **No** |
| Switches to next thread | Yes | Yes |
| **Effect on dead process** | Gets re-scheduled → crash | **Vanishes from scheduler** |

**SIGKILL is handled in two places:**

```mermaid
graph TD
    subgraph "Path 1: sys_kill (TSyscall.c)"
        A["Another process calls<br/>kill(target, SIGKILL)"]
        B["sys_kill detects SIGKILL"]
        C["Set target TSTATE_DEAD<br/>Remove from queue<br/>Clear pending signals"]
    end

    subgraph "Path 2: handle_pending_signals (TTrapHandler.c)"
        D["Target process is about<br/>to return to user mode"]
        E["Check pending signals:<br/>SIGKILL bit is set"]
        F["terminate_process(cur_pid)"]
        G["thread_exit()<br/>Switch to next process"]
    end

    A --> B --> C
    D --> E --> F --> G
```

- **Path 1:** Another process kills the target — immediate from sender's context
- **Path 2:** Self-kill or deferred — when checking pending signals in trap return path

---

## Slide 15: Implementation Roadmap — 7 Phases

```mermaid
graph TD
    A["Phase 1: Data Structures<br/>kern/lib/signal.h ✦NEW✦<br/>kern/lib/syscall.h (enums)<br/>kern/lib/thread.h (struct)"] --> B

    B["Phase 2: Storage Layer<br/>PTCBIntro.c (sigstate + accessors)<br/>PTCBIntro/export.h (declarations)"] --> C
    B --> D

    C["Phase 3: Thread Exit<br/>PThread.c (thread_exit)<br/>PThread/export.h"] --> F

    D["Phase 4: Syscall Implementations<br/>TSyscall.c (4 functions)<br/>TDispatch.c (4 case entries)<br/>TDispatch/import.h"] --> F

    F["Phase 5: Signal Delivery Engine<br/>TTrapHandler.c<br/>deliver_signal + handle_pending_signals<br/>+ trap() modification"]

    G["Phase 6: User Space<br/>user/include/signal.h ✦NEW✦<br/>user/include/syscall.h (wrappers)<br/>user/lib/signal.c ✦NEW✦<br/>user/lib/Makefile.inc"] --> H

    H["Phase 7: Shell & Testing<br/>user/shell/shell.c<br/>user/signal_test.c<br/>user/Makefile.inc"]

    A --> G

    style A fill:#ccffcc
    style F fill:#ff9999,stroke:#cc0000,stroke-width:3px
    style G fill:#ccccff
```

- **Phase 5 (red)** is the most complex — save it until all dependencies compile
- **Phases 1→6 are parallelizable** (kernel and user space are independently compilable)
- Run `make` after each phase to catch compiler errors early

---

## Slide 16: Phase Details & File Map

| Phase | Files Modified/Created | Lines | Key Deliverable |
|-------|----------------------|-------|-----------------|
| 1 — Data Structures | `kern/lib/signal.h` ✦, `syscall.h`, `thread.h` | ~85 | Signal types, constants, enums |
| 2 — Storage Layer | `PTCBIntro.c`, `PTCBIntro/export.h` | ~105 | `sig_state` in TCB + 12 accessor functions |
| 3 — Thread Exit | `PThread.c`, `PThread/export.h` | ~30 | `thread_exit()` — exit without re-queuing |
| 4 — Syscall Impl | `TSyscall.c`, `TDispatch.c`, `TDispatch/import.h` | ~180 | `sys_sigaction`, `sys_kill`, `sys_pause`, `sys_sigreturn` |
| 5 — Delivery Engine | `TTrapHandler.c` | ~145 | `deliver_signal`, `handle_pending_signals`, `trap()` mod |
| 6 — User API | `user/include/signal.h` ✦, `syscall.h`, `signal.c` ✦, `Makefile.inc` | ~120 | POSIX wrappers + inline asm stubs |
| 7 — Shell & Test | `shell.c`, `signal_test.c`, `Makefile.inc` | ~240 | `kill`, `trap`, `spawn` commands + test program |

**Total: 3 new files + 16 modified files ≈ 760 lines of new code**

---

## Slide 17: Key Design Decisions & Rationale

| Decision | Why |
|----------|-----|
| **Bitmask for pending signals** | O(1) set/clear/check; matches Linux standard signal model |
| **`pt_copyin`/`pt_copyout` for user data** | Kernel PT ≠ User PT; direct pointer dereference crashes |
| **Inline trampoline on stack** | Self-contained; no fixed addresses or user-lib dependencies |
| **cdecl for handler args** | GCC compiles handlers as normal C functions → args on stack |
| **Signal check before `set_pdir_base`** | `pt_copyout` needs identity-mapped physical memory (kernel PT) |
| **`thread_exit()` for SIGKILL** | `thread_yield()` re-enqueues → dead process gets re-scheduled |
| **Store addresses, not values** | Saved context lives on user stack like real OS implementations |
| **12 TCB accessor functions** | Maintains CertiKOS layered verification boundaries |

---

## Slide 18: End-to-End Signal Lifecycle

```mermaid
sequenceDiagram
    participant Shell as Shell Process
    participant Kernel as Kernel (sys_kill)
    participant TCB as Target TCB
    participant Trap as trap() return path
    participant Stack as User Stack
    participant Handler as Signal Handler
    participant Trampoline as Trampoline Code
    participant Sigreturn as sys_sigreturn

    Shell->>Kernel: kill(pid, SIGUSR1)
    Kernel->>TCB: Set bit 10 in pending_signals

    Note over Trap: Later, when target process<br/>is about to return to user mode...

    Trap->>TCB: Check pending_signals != 0
    Trap->>TCB: Get sigaction for SIGUSR1
    Trap->>Stack: Push saved EIP, saved ESP
    Trap->>Stack: Push trampoline code (machine code)
    Trap->>Stack: Push signum (10)
    Trap->>Stack: Push return addr (→ trampoline)
    Trap->>Trap: tf->eip = handler address

    Note over Trap: IRET executes →<br/>CPU jumps to handler

    Handler->>Handler: printf("got signal %d", 10)
    Handler->>Handler: return (pops ret addr → trampoline)

    Trampoline->>Trampoline: mov eax, SYS_sigreturn
    Trampoline->>Trampoline: int 0x30

    Trampoline->>Sigreturn: Enters kernel
    Sigreturn->>TCB: Read saved_esp_addr, saved_eip_addr
    Sigreturn->>Stack: Read original ESP, EIP from stack
    Sigreturn->>Trap: Restore tf->esp, tf->eip

    Note over Trap: IRET executes →<br/>CPU resumes original code
```

---

## Slide 19: Thank You

### Summary

- **7-phase implementation** touching kernel data structures, thread management, trap handling, and user-space API
- **Core mechanism:** Trap frame hijack + inline trampoline + `sigreturn` restoration
- **Critical constraint:** Signal delivery must occur under the kernel page table (before `set_pdir_base`)


---
