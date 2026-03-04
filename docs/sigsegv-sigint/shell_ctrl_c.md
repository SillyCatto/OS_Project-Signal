# Shell Ctrl+C: The 9-Layer Journey — Complete Technical Reference

## Table of Contents

- [Shell Ctrl+C: The 9-Layer Journey — Complete Technical Reference](#shell-ctrlc-the-9-layer-journey--complete-technical-reference)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Design: What Happens When You Press Ctrl+C](#design-what-happens-when-you-press-ctrlc)
  - [Layer 1: PS/2 Keyboard Hardware](#layer-1-ps2-keyboard-hardware)
  - [Layer 2: Keyboard Driver — Scancode to ASCII](#layer-2-keyboard-driver--scancode-to-ascii)
    - [Modifier Key Tracking](#modifier-key-tracking)
    - [Processing a Scancode](#processing-a-scancode)
    - [The Character Map Selection for Ctrl+C](#the-character-map-selection-for-ctrlc)
    - [The C(x) Macro and Control Characters](#the-cx-macro-and-control-characters)
  - [Layer 3: Console Ring Buffer — Interrupt-Safe Storage](#layer-3-console-ring-buffer--interrupt-safe-storage)
    - [Why a Ring Buffer?](#why-a-ring-buffer)
    - [Data Structure](#data-structure)
    - [Producer: cons\_intr() — Called from Keyboard Interrupt](#producer-cons_intr--called-from-keyboard-interrupt)
    - [Consumer: cons\_getc() — Called from getchar()](#consumer-cons_getc--called-from-getchar)
    - [Ring Buffer State Visualization](#ring-buffer-state-visualization)
    - [Why the Spinlock is Necessary](#why-the-spinlock-is-necessary)
  - [Layer 4: getchar() — Polling with Cooperative Yield](#layer-4-getchar--polling-with-cooperative-yield)
    - [When No Key Is Being Pressed (Normal Timesharing)](#when-no-key-is-being-pressed-normal-timesharing)
    - [When Ctrl+C Is Pressed](#when-ctrlc-is-pressed)
    - [Why thread\_yield() Is Here](#why-thread_yield-is-here)
  - [Layer 5: readline() — Ctrl+C Detection and NULL Return](#layer-5-readline--ctrlc-detection-and-null-return)
    - [Why 0x03 Doesn't Pass the Normal Character Filter](#why-0x03-doesnt-pass-the-normal-character-filter)
    - [Why Print `^C`?](#why-print-c)
    - [Why Return NULL?](#why-return-null)
  - [Layer 6: sys\_readline() — Signal Dispatch in the Kernel](#layer-6-sys_readline--signal-dispatch-in-the-kernel)
    - [What `tcb_add_pending_signal()` Does](#what-tcb_add_pending_signal-does)
    - [Why the Signal Isn't Delivered Here](#why-the-signal-isnt-delivered-here)
    - [What Happens to the User-Space Return](#what-happens-to-the-user-space-return)
  - [Layer 7: trap() → handle\_pending\_signals() → deliver\_signal()](#layer-7-trap--handle_pending_signals--deliver_signal)
    - [The trap() Function — Signal Check Point](#the-trap-function--signal-check-point)
    - [handle\_pending\_signals() — The Signal Dispatcher](#handle_pending_signals--the-signal-dispatcher)
    - [deliver\_signal() — The Trapframe Rewrite](#deliver_signal--the-trapframe-rewrite)
  - [Layer 8: User-Space Signal Handler Execution](#layer-8-user-space-signal-handler-execution)
    - [What `kill(foreground_pid, SIGKILL)` Does](#what-killforeground_pid-sigkill-does)
  - [Layer 9: Trampoline → sigreturn → Context Restoration](#layer-9-trampoline--sigreturn--context-restoration)
    - [Step 1: Handler Returns — CPU Jumps to Trampoline](#step-1-handler-returns--cpu-jumps-to-trampoline)
    - [Step 2: Trampoline Executes](#step-2-trampoline-executes)
    - [Step 3: sys\_sigreturn() — Kernel Restores Context](#step-3-sys_sigreturn--kernel-restores-context)
    - [Step 4: CPU Resumes at Original Location](#step-4-cpu-resumes-at-original-location)
  - [Complete Timing Analysis: What Happens When](#complete-timing-analysis-what-happens-when)
  - [The Scheduling Dance: Shell and Child Timesharing](#the-scheduling-dance-shell-and-child-timesharing)
    - [CertiKOS Scheduling Model](#certikos-scheduling-model)
    - [Ready Queue Structure](#ready-queue-structure)
    - [Interleaving Pattern](#interleaving-pattern)
  - [The Foreground Process Model](#the-foreground-process-model)
    - [CertiKOS Implementation](#certikos-implementation)
    - [Comparison with Real UNIX Job Control](#comparison-with-real-unix-job-control)
  - [Signal Stack Frame: Byte-Level Layout](#signal-stack-frame-byte-level-layout)
    - [How the x86 cdecl Calling Convention Uses This](#how-the-x86-cdecl-calling-convention-uses-this)
  - [Three Bugs and Their Fixes](#three-bugs-and-their-fixes)
    - [Bug 1: Ready Queue Corruption](#bug-1-ready-queue-corruption)
    - [Bug 2: Child Process Starvation](#bug-2-child-process-starvation)
    - [Bug 3: Unknown Command After Ctrl+C](#bug-3-unknown-command-after-ctrlc)
  - [Differences from Real UNIX](#differences-from-real-unix)
    - [What CertiKOS Gets Right](#what-certikos-gets-right)
  - [Complete File Inventory](#complete-file-inventory)
    - [Files Created](#files-created)
    - [Files Modified](#files-modified)
    - [Files Referenced (Not Modified)](#files-referenced-not-modified)
  - [All Constants and Definitions](#all-constants-and-definitions)

---

## Overview

This document traces the complete path of a Ctrl+C keypress from the physical keyboard hardware through every layer of the CertiKOS kernel to the user-space signal handler and back. There are **9 distinct layers** involved, spanning hardware ports, interrupt handling, device drivers, ring buffers, scheduling, signal dispatch, trapframe manipulation, and user-space handler execution.

This is the most complex cross-cutting operation in our signal implementation — it touches more kernel subsystems than any other single feature.

---

## Design: What Happens When You Press Ctrl+C

When a foreground process (like `sigint_test` printing infinitely) is running and the user presses Ctrl+C, the following complete sequence occurs:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                       THE 9-LAYER Ctrl+C JOURNEY                             │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Layer 1   PS/2 Keyboard → scancode 0x2E with CTL modifier                   │
│      ↓                                                                       │
│  Layer 2   kbd_proc_data() → ctlmap[0x2E] = C('C') = 0x03                    │
│      ↓                                                                       │
│  Layer 3   cons_intr() → cons.buf[wpos++] = 0x03 (ring buffer)               │
│      ↓                                                                       │
│  Layer 4   getchar() → cons_getc() → returns 0x03                            │
│      ↓                                                                       │
│  Layer 5   readline() detects 0x03 → prints "^C\n" → returns NULL            │
│      ↓                                                                       │
│  Layer 6   sys_readline() → tcb_add_pending_signal(pid, SIGINT)              │
│      ↓                                                                       │
│  Layer 7   trap() → handle_pending_signals() → deliver_signal(tf, 2)         │
│      ↓                                                                       │
│  Layer 8   Shell's sigint_handler(2) runs in user mode                       │
│            → kill(foreground_pid, foreground_signal) → child dies or catches   │
│      ↓                                                                       │
│  Layer 9   Handler returns → trampoline → sigreturn                          │
│            → restore ESP/EIP → shell back at readline → ">:" prompt          │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Layer 1: PS/2 Keyboard Hardware

**Where**: Physical keyboard → Intel 8042 keyboard controller → CPU I/O ports

The PS/2 keyboard is connected to the Intel 8042 keyboard controller, which interfaces with the CPU via I/O ports:

| Port | Name | Direction | Purpose |
|------|------|-----------|---------|
| `0x64` | `KBSTATP` | Read | Status register — bit 0 (`KBS_DIB`) = data available in buffer |
| `0x60` | `KBDATAP` | Read | Data register — the raw scancode byte |

When a physical key is pressed or released, the keyboard controller:
1. Translates the key matrix position into a **scancode** (Set 1 — IBM XT compatible)
2. Stores the scancode in its internal buffer
3. Sets `KBS_DIB` (Data In Buffer) bit in the status register
4. Optionally raises IRQ 1 (keyboard interrupt)

**For Ctrl+C, the hardware generates this scancode sequence:**

| Step | Physical Event | Scancode | Hex | Type |
|------|---------------|----------|-----|------|
| 1 | User presses Ctrl | `0x1D` | `1D` | Make code (key down) |
| 2 | User presses C | `0x2E` | `2E` | Make code (key down) |
| 3 | User releases C | `0xAE` | `AE` | Break code (`0x2E | 0x80`) |
| 4 | User releases Ctrl | `0x9D` | `9D` | Break code (`0x1D | 0x80`) |

**Break codes** (key releases) have bit 7 set — this is the `data & 0x80` check in `kbd_proc_data()`. They're used to clear modifier flags but don't produce characters.

**How the scancode is read by the CPU:**

```c
// In kbd_proc_data() — kern/dev/keyboard.c

if ((inb(KBSTATP) & KBS_DIB) == 0)  // Check: data available?
    return -1;                        // No → return "no data"

data = inb(KBDATAP);                 // Yes → read the scancode byte
```

`inb()` is an x86 I/O port read instruction (`in al, dx`). The CPU reads one byte from the specified port.

---

## Layer 2: Keyboard Driver — Scancode to ASCII

**File**: `kern/dev/keyboard.c` — `kbd_proc_data()`

The keyboard driver converts raw scancodes into ASCII characters using a state machine and lookup tables.

### Modifier Key Tracking

The driver maintains a static `shift` variable that tracks which modifier keys are currently held:

```c
#define SHIFT   (1<<0)   // Bit 0: Shift key
#define CTL     (1<<1)   // Bit 1: Control key
#define ALT     (1<<2)   // Bit 2: Alt key
#define CAPSLOCK  (1<<3) // Bit 3: Caps Lock toggle
#define NUMLOCK   (1<<4) // Bit 4: Num Lock toggle
#define SCROLLLOCK (1<<5)// Bit 5: Scroll Lock toggle
#define E0ESC   (1<<6)   // Bit 6: Extended scancode prefix (E0)
```

The `shiftcode[]` table maps modifier key scancodes to their flag bits:

```c
static uint8_t shiftcode[256] = {
    [0x1D] = CTL,    // Left Control press → set CTL bit
    [0x2A] = SHIFT,  // Left Shift press   → set SHIFT bit
    [0x36] = SHIFT,  // Right Shift press  → set SHIFT bit
    [0x38] = ALT,    // Left Alt press     → set ALT bit
    [0x9D] = CTL,    // Left Control release → clear CTL bit
    [0xB8] = ALT,    // Left Alt release   → clear ALT bit
};
```

### Processing a Scancode

```c
static int kbd_proc_data(void)
{
    int c;
    uint8_t data;
    static uint32_t shift;     // Persistent across calls!

    if ((inb(KBSTATP) & KBS_DIB) == 0)
        return -1;             // No data available
    data = inb(KBDATAP);      // Read scancode

    // Handle E0 extended scancodes (for numpad, arrow keys, etc.)
    if (data == 0xE0) {
        shift |= E0ESC;
        return 0;              // Will process next byte
    }

    // Handle key RELEASE (break code — bit 7 set)
    if (data & 0x80) {
        data = (shift & E0ESC ? data : data & 0x7F);
        shift &= ~(shiftcode[data] | E0ESC);  // Clear modifier flags
        return 0;              // No character to produce
    }

    // Handle E0-prefixed keys (OR with 0x80 to use upper half of tables)
    if (shift & E0ESC) {
        data |= 0x80;
        shift &= ~E0ESC;
    }

    // Track modifier state
    shift |= shiftcode[data];      // Set modifier bits for make codes
    shift ^= togglecode[data];      // Toggle Caps/Num/Scroll Lock

    // LOOK UP THE CHARACTER using the correct keymap
    c = charcode[shift & (CTL | SHIFT)][data];

    // Handle Caps Lock
    if (shift & CAPSLOCK) {
        if ('a' <= c && c <= 'z') c += 'A' - 'a';
        else if ('A' <= c && c <= 'Z') c += 'a' - 'A';
    }

    return c;                  // Return the final ASCII character
}
```

### The Character Map Selection for Ctrl+C

The `charcode[]` array holds 4 keymap pointers, indexed by the two modifier bits:

```c
static uint8_t *charcode[4] = {
    normalmap,   // index 0: no modifiers      (shift=0b00)
    shiftmap,    // index 1: SHIFT only         (shift=0b01)
    ctlmap,      // index 2: CTL only           (shift=0b10)
    ctlmap       // index 3: CTL + SHIFT        (shift=0b11)
};
```

When Ctrl+C is pressed:
1. Ctrl down (scancode `0x1D`): `shift |= CTL` → shift has bit 1 set
2. C down (scancode `0x2E`): `charcode[shift & (CTL|SHIFT)]` = `charcode[0b10]` = `ctlmap`
3. `ctlmap[0x2E]` = `C('C')`

### The C(x) Macro and Control Characters

```c
#define C(x) (x - '@')
```

This is the universal ASCII control character formula. Every printable letter in ASCII has a corresponding control character:

```
C('C') = 'C' - '@' = 67 - 64 = 3 = 0x03  (ETX)
```

`0x03` is the ASCII **ETX** (End of Text) character. This is the canonical representation of Ctrl+C across all UNIX-like operating systems and terminals.

The full `ctlmap` entry for the key at scancode `0x2E`:

```c
static uint8_t ctlmap[256] = {
    // ... (rows 0x00–0x18)
    C('Q'),  C('W'),  C('E'),  C('R'),  C('T'),  C('Y'),  C('U'),  C('I'),  // 0x10
    C('O'),  C('P'),  NO,      NO,      '\r',    NO,      C('A'),  C('S'),  // 0x18 (cont)
    C('D'),  C('F'),  C('G'),  C('H'),  C('J'),  C('K'),  C('L'),  NO,      // 0x20
    NO,      NO,      NO,      C('\\'), C('Z'),  C('X'),  C('C'),  C('V'),  // 0x28
    //                                                      ↑ 0x2E = C('C') = 3
};
```

**Result**: `kbd_proc_data()` returns `3` (0x03) — the ASCII control character for Ctrl+C.

---

## Layer 3: Console Ring Buffer — Interrupt-Safe Storage

**File**: `kern/dev/console.c`

### Why a Ring Buffer?

The keyboard produces characters at interrupt time (asynchronously), but processes consume them by polling (synchronously). A ring buffer decouples these two flows.

### Data Structure

```c
spinlock_t cons_lk;  // Mutual exclusion for the buffer

struct {
    char buf[CONSOLE_BUFFER_SIZE];  // 512-byte circular buffer
    uint32_t rpos;                  // Read position (consumer advances)
    uint32_t wpos;                  // Write position (producer advances)
} cons;
```

### Producer: cons_intr() — Called from Keyboard Interrupt

```c
void cons_intr(int (*proc)(void))
{
    int c;
    spinlock_acquire(&cons_lk);

    while ((c = (*proc)()) != -1) {  // proc = kbd_proc_data
        if (c == 0) continue;        // Skip null (from modifier-only events)
        cons.buf[cons.wpos++] = c;   // Store character at write position
        if (cons.wpos == CONSOLE_BUFFER_SIZE)
            cons.wpos = 0;           // Wrap around (circular)
    }

    spinlock_release(&cons_lk);
}
```

**For Ctrl+C:**
- `kbd_proc_data()` returns `3` (0x03) for the C key with CTL held
- `cons.buf[wpos] = 3; wpos++`
- The 0x03 byte is now sitting in the ring buffer, waiting to be consumed

**What about the Ctrl key itself?**
- Scancode `0x1D` (Ctrl press) only updates `shift` bits — `kbd_proc_data()` returns `0` for it
- The `if (c == 0) continue;` statement skips it — modifiers don't produce buffer entries
- Same for the break codes (key releases): they return `0` and are skipped

### Consumer: cons_getc() — Called from getchar()

```c
char cons_getc(void)
{
    int c;

    serial_intr();       // Poll serial port
    keyboard_intr();     // Poll keyboard (calls cons_intr(kbd_proc_data))

    spinlock_acquire(&cons_lk);

    if (cons.rpos != cons.wpos) {       // Is there data in the buffer?
        c = cons.buf[cons.rpos++];      // Read next character
        if (cons.rpos == CONSOLE_BUFFER_SIZE)
            cons.rpos = 0;              // Wrap around
        spinlock_release(&cons_lk);
        return c;                       // Return the character
    }

    spinlock_release(&cons_lk);
    return 0;                           // Buffer empty → no character
}
```

**For Ctrl+C:**
- `keyboard_intr()` calls `cons_intr(kbd_proc_data)`, which polls the keyboard controller
  - If the scancode hasn't been buffered yet (e.g., entering via interrupt), this poll retrieves it
  - If it was already buffered by a previous interrupt, the poll returns -1 (no new data)
- `rpos != wpos` → true (the 0x03 is in the buffer)
- `c = cons.buf[rpos] = 3; rpos++`
- Returns `3` (0x03) to the caller

### Ring Buffer State Visualization

```
Before Ctrl+C (buffer empty):
cons.buf: [  ] [  ] [  ] [  ] ... [   ]
              ↑
           rpos = wpos  (empty)

After keyboard interrupt stores 0x03:
cons.buf: [  ] [03] [  ] [  ] ... [   ]
              ↑    ↑
            rpos  wpos  (one byte available)

After cons_getc() reads 0x03:
cons.buf: [  ] [03] [  ] [  ] ... [   ]
                    ↑
              rpos = wpos  (empty again)
```

### Why the Spinlock is Necessary

On a multiprocessor system (CertiKOS supports SMP):
- **CPU 0**: running the shell, calling `cons_getc()` → reading from the buffer
- **CPU 1**: receives keyboard IRQ → `keyboard_intr()` → `cons_intr()` → writing to the buffer

Without `cons_lk`, a race condition could occur:
1. CPU 0 reads `cons.rpos != cons.wpos` — sees data available
2. CPU 1 interrupts, writes another byte, advances `wpos`
3. CPU 0 reads `cons.buf[rpos]` — gets the wrong byte or corrupted index

The spinlock ensures mutual exclusion between producers and consumers.

---

## Layer 4: getchar() — Polling with Cooperative Yield

**File**: `kern/dev/console.c`

```c
extern void thread_yield(void);

int getchar(void)
{
    int c;
    while ((c = cons_getc()) == 0)
        thread_yield();  /* yield CPU so other processes can run */
    return c;
}
```

This is the critical polling loop. Here's what happens moment-by-moment:

### When No Key Is Being Pressed (Normal Timesharing)

```
Iteration 1:
  cons_getc() → keyboard_intr() → no scancode available → rpos == wpos → returns 0
  thread_yield() → shell enqueued as READY → child dequeued → child runs printf loop

  [Child prints "Hello World! (0)", "Hello World! (1)", ... for ~5ms]

  Timer IRQ → sched_update() → SCHED_SLICE exceeded → thread_yield()
  Child enqueued → Shell dequeued → Shell resumes in getchar()

Iteration 2:
  cons_getc() → keyboard_intr() → no scancode → rpos == wpos → returns 0
  thread_yield() → shell → child again

  [This alternation continues — shell checks keyboard, child prints output]
```

### When Ctrl+C Is Pressed

```
Iteration N:
  cons_getc() → keyboard_intr() → kbd_proc_data()
    → inb(0x64): KBS_DIB set (data available!)
    → inb(0x60): data = 0x2E
    → shift has CTL set → charcode[2][0x2E] = C('C') = 3
    → cons_intr: cons.buf[wpos++] = 3
  → back in cons_getc: rpos != wpos → c = cons.buf[rpos++] = 3
cons_getc returns 3 (not 0!)
while loop exits
getchar returns 3 (0x03)
```

### Why thread_yield() Is Here

This was added to fix the **child process starvation** bug. See [Bug 2: Child Process Starvation](#bug-2-child-process-starvation) for the full story.

The `extern` declaration is necessary because `console.c` is in the device layer (`kern/dev/`) and doesn't include thread headers. Using `extern` avoids circular include dependencies between the device and thread layers.

---

## Layer 5: readline() — Ctrl+C Detection and NULL Return

**File**: `kern/dev/console.c`

```c
char *readline(const char *prompt)
{
    int i;
    char c;

    if (prompt != NULL)
        dprintf("%s", prompt);   // Print ">:" (shell prompt)

    i = 0;
    while (1) {
        c = getchar();           // Block until a character is available (Layer 4)

        if (c < 0) {
            dprintf("read error: %e\n", c);
            return NULL;
        } else if (c == 0x03) {
            /* Ctrl+C: print ^C\n and return NULL to signal interruption */
            putchar('^');
            putchar('C');
            putchar('\n');
            return NULL;         // ← This NULL propagates to sys_readline()
        } else if ((c == '\b' || c == '\x7f') && i > 0) {
            putchar('\b');       // Backspace handling
            i--;
        } else if (c >= ' ' && i < BUFLEN-1) {
            putchar(c);          // Normal printable character
            linebuf[i++] = c;
        } else if (c == '\n' || c == '\r') {
            putchar('\n');       // Enter — line complete
            linebuf[i] = 0;
            return linebuf;      // ← Normal return path
        }
    }
}
```

### Why 0x03 Doesn't Pass the Normal Character Filter

Control characters have ASCII values 0x00–0x1F. The printable character branch requires `c >= ' '` (0x20). Since 0x03 < 0x20, it would be silently ignored without the explicit `c == 0x03` check.

The check for 0x03 is placed **before** the normal character filter in the if-else chain, ensuring Ctrl+C is caught first.

### Why Print `^C`?

This is the universal UNIX convention for displaying control characters in terminals. When a control character is "echoed," it's shown as a caret (`^`) followed by the corresponding uppercase letter. `^C` means "Control-C was pressed." This visual feedback is important because:
1. The user sees confirmation that their Ctrl+C was received
2. It indicates the point in the terminal output where the interruption occurred
3. The newline after `^C` moves the cursor to a fresh line for the next prompt

### Why Return NULL?

Three alternative designs were considered:

| Approach | Problem |
|----------|---------|
| Return empty string `""` | Ambiguous — user could also press Enter immediately |
| Return special string `"^C"` | Caller must do string comparison; fragile |
| Set a global flag | Not thread-safe; adds coupling between console and syscall layers |
| **Return NULL** | **Unambiguous, type-safe, follows POSIX convention** |

In POSIX, `read()` returns 0 on EOF and -1 on error/interrupt. Returning NULL for an interrupted `readline()` follows this spirit — NULL is the "exceptional" return value.

---

## Layer 6: sys_readline() — Signal Dispatch in the Kernel

**File**: `kern/trap/TSyscall/TSyscall.c`

`sys_readline()` is the kernel-side handler for the `SYS_readline` syscall. This is where the NULL from `readline()` gets converted into a signal.

```c
void sys_readline(tf_t *tf)
{
    char *kernbuf = (char *)readline(">:");

    /* readline returns NULL when interrupted by Ctrl+C */
    if (kernbuf == NULL) {
        unsigned int cur_pid = get_curid();

        /* 1. Mark SIGINT as pending */
        tcb_add_pending_signal(cur_pid, SIGINT);

        /* 2. Set error return values */
        syscall_set_errno(tf, E_INVAL_EVENT);
        syscall_set_retval1(tf, -1);

        /* 3. Copy empty string to user buffer (prevent stale data processing) */
        char *userbuf = (char *)syscall_get_arg2(tf);
        char empty = '\0';
        pt_copyout((void *)&empty, cur_pid, userbuf, 1);
        return;
    }

    /* Normal path: copy completed line to user buffer */
    char *userbuf = (char *)syscall_get_arg2(tf);
    int n_len = strnlen(kernbuf, 1000) + 1;
    if (pt_copyout((void *)kernbuf, get_curid(), userbuf, n_len) != n_len) {
        KERN_PANIC("Readline fails!\n");
    }
    syscall_set_errno(tf, E_SUCC);
    syscall_set_retval1(tf, 0);
}
```

### What `tcb_add_pending_signal()` Does

```c
// In kern/thread/PTCBIntro/PTCBIntro.c
void tcb_add_pending_signal(unsigned int pid, int signum) {
    TCBPool[pid].sigstate.pending_signals |= (1 << signum);
}
```

For SIGINT (signal 2): `pending_signals |= (1 << 2)` = `pending_signals |= 0x00000004`

The `pending_signals` is a 32-bit bitmask where each bit represents one signal number. This is why NSIG is 32 — one bit per signal, fitting in a single uint32_t.

```
Bit:  31 30 29 ... 11 ... 9 ... 2  1  0
Sig:  31 30 29 ... 11 ... 9 ... 2  1  -
      SIGSEGV   SIGKILL  SIGINT
                                  ↑
                            Set by tcb_add_pending_signal(pid, SIGINT)
```

### Why the Signal Isn't Delivered Here

The signal is only **marked as pending**. The actual delivery (trapframe rewriting) happens in `handle_pending_signals()`, which is called from `trap()` just before returning to user space. This separation is important:

1. `sys_readline()` runs in the context of handling a syscall trap
2. The trapframe `tf` is being actively used by the syscall handler
3. Modifying `tf->eip` and `tf->esp` here would interfere with the syscall return values being set
4. `handle_pending_signals()` runs after all trap processing is complete, just before `trap_return(tf)`

### What Happens to the User-Space Return

The user-space `sys_readline()` wrapper (`user/include/syscall.h`):

```c
static gcc_inline int sys_readline(char* start)
{
    int errno, ret;
    asm volatile("int %2"
         : "=a" (errno),       // Output: EAX → errno
           "=b" (ret)          // Output: EBX → ret
         : "i" (T_SYSCALL),    // Input: trap number 0x30
           "a" (SYS_readline), // Input: EAX = syscall number
           "b" (start)         // Input: EBX = buffer pointer
         : "cc", "memory");
    return errno ? -1 : 0;
}
```

The kernel set `errno = E_INVAL_EVENT` (non-zero), so:
- `return errno ? -1 : 0` → `return true ? -1 : 0` → **returns -1**

**BUT**: By the time this inline asm executes in user mode, the signal handler has already run (deliver_signal rewrote the trapframe). After `sigreturn` restores the original EIP/ESP, the code resumes as if the `int 0x30` just returned. The EAX register at this point contains whatever value was left after sigreturn — which is the `E_SUCC` set by `sys_sigreturn()`. This is the root cause of [Bug 3](#bug-3-unknown-command-after-ctrlc).

---

## Layer 7: trap() → handle_pending_signals() → deliver_signal()

**File**: `kern/trap/TTrapHandler/TTrapHandler.c`

### The trap() Function — Signal Check Point

After `sys_readline()` returns, execution continues in `trap()`:

```c
void trap(tf_t *tf)
{
    unsigned int cur_pid;
    cur_pid = get_curid();
    set_pdir_base(0);           // Switch to kernel page table

    // 1. Dispatch to the registered handler (e.g., sys_readline for syscall)
    trap_cb_t f = TRAP_HANDLER[get_pcpu_idx()][tf->trapno];
    if (f) {
        f(tf);                   // ← sys_readline() runs here
    }

    // 2. Switch to the process's kernel stack
    kstack_switch(cur_pid);

    // 3. CHECK FOR PENDING SIGNALS (this is the key moment)
    handle_pending_signals(tf);

    // 4. Switch to user page table
    set_pdir_base(cur_pid);

    // 5. Return to user space
    trap_return(tf);             // ← iret: restores EIP, ESP, EFLAGS, CS, SS
}
```

**Why signals are checked BEFORE `set_pdir_base(cur_pid)`:**

`handle_pending_signals()` may call `deliver_signal()`, which uses `pt_copyout()` to write data (trampoline code, saved ESP/EIP) to the user's stack. `pt_copyout()` accesses user memory through the kernel's page table using identity-mapped physical addresses. If we switched to the user's page table first, the kernel's `pt_copyout()` function might not work correctly because it needs to traverse its own page table structures.

### handle_pending_signals() — The Signal Dispatcher

```c
static void handle_pending_signals(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t pending_signals = tcb_get_pending_signals(cur_pid);

    if (pending_signals != 0) {
        for (int signum = 1; signum < NSIG; signum++) {
            if (pending_signals & (1 << signum)) {
                // Clear the pending bit
                tcb_clear_pending_signal(cur_pid, signum);

                // SIGKILL: uncatchable → immediate termination
                if (signum == SIGKILL) {
                    terminate_process(cur_pid);
                    thread_exit();
                    return;
                }

                // Check for user-registered handler
                struct sigaction *sa = tcb_get_sigaction(cur_pid, signum);
                if (sa != NULL && sa->sa_handler != NULL) {
                    deliver_signal(tf, signum);  // ← Rewrite trapframe
                } else {
                    // No handler → default action (terminate)
                    terminate_process(cur_pid);
                    thread_exit();
                    return;
                }
                break;  // Process one signal per trap return
            }
        }
    }
}
```

**For SIGINT with handler registered:**
- `pending_signals = 0x00000004` (bit 2 set)
- Loop reaches `signum = 2` → bit check passes
- Clears bit: `pending_signals &= ~(1 << 2)` → `0x00000000`
- SIGKILL check: 2 != 9 → no
- `tcb_get_sigaction(cur_pid, 2)` returns the shell's registered handler → `sa_handler = sigint_handler`
- Calls `deliver_signal(tf, 2)`

**Why only one signal per trap return (`break`)?**

Processing multiple signals in one `handle_pending_signals` call would require nesting signal stack frames, which adds complexity. By processing one at a time and breaking, we ensure each signal gets a clean delivery and return path. If multiple signals are pending, the next one will be processed on the next trap return.

### deliver_signal() — The Trapframe Rewrite

This is the most intricate function in the entire signal implementation. It modifies the process's user-space stack and trapframe so that when `trap_return(tf)` executes `iret`, the CPU jumps to the signal handler instead of the original code.

```c
static void deliver_signal(tf_t *tf, int signum)
{
    unsigned int cur_pid = get_curid();
    struct sigaction *sa = tcb_get_sigaction(cur_pid, signum);

    if (sa != NULL && sa->sa_handler != NULL) {
        uint32_t orig_esp = tf->esp;   // Save original stack pointer
        uint32_t orig_eip = tf->eip;   // Save original instruction pointer
        uint32_t new_esp = orig_esp;

        // ---- BUILD THE SIGNAL FRAME ON THE USER STACK ----

        // 1. Save original EIP (for sigreturn to restore later)
        new_esp -= 4;
        pt_copyout(&orig_eip, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_eip_addr = new_esp;

        // 2. Save original ESP (for sigreturn to restore later)
        new_esp -= 4;
        pt_copyout(&orig_esp, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_esp_addr = new_esp;

        // 3. Write trampoline machine code (12 bytes)
        uint8_t trampoline[12] = {
            0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,   // mov eax, SYS_sigreturn
            0xCD, 0x30,                                // int 0x30
            0xEB, 0xFE,                                // jmp $ (infinite loop — safety)
            0x90, 0x90, 0x90                           // nop nop nop (padding)
        };
        new_esp -= 12;
        pt_copyout(trampoline, cur_pid, new_esp, 12);
        uint32_t trampoline_addr = new_esp;

        // 4. Align stack to 4-byte boundary
        new_esp = new_esp & ~3;

        // 5. Push signal number (first argument to handler function)
        new_esp -= 4;
        uint32_t sig_arg = signum;     // signum = 2 (SIGINT)
        pt_copyout(&sig_arg, cur_pid, new_esp, sizeof(uint32_t));

        // 6. Push return address (points to trampoline code above)
        new_esp -= 4;
        pt_copyout(&trampoline_addr, cur_pid, new_esp, sizeof(uint32_t));

        // ---- REWRITE THE TRAPFRAME ----

        tf->esp = new_esp;                         // New stack pointer
        tf->eip = (uint32_t)sa->sa_handler;        // Jump to handler on return

        // ---- SAVE CONTEXT LOCATIONS FOR SIGRETURN ----

        tcb_set_signal_context(cur_pid, saved_esp_addr, saved_eip_addr);
    }
}
```

After `deliver_signal()` returns, `trap()` continues:
- `set_pdir_base(cur_pid)` → switch to user's page table
- `trap_return(tf)` → x86 `iret` → CPU restores `tf->eip` (= `sigint_handler`), `tf->esp` (= `new_esp`)
- **User code now executing**: `sigint_handler()` with the fabricated stack frame

---

## Layer 8: User-Space Signal Handler Execution

**File**: `user/shell/shell.c`

When the CPU enters `sigint_handler()`, it's running in user mode (ring 3) with:
- `EIP` = address of `sigint_handler`
- `ESP` = `new_esp` (pointing to the fabricated call frame on the stack)
- `[ESP]` = trampoline_addr (return address)
- `[ESP+4]` = 2 (signal number — first function argument)

```c
void sigint_handler(int signum)
{
    if (foreground_pid > 0) {
        printf("\n[SHELL] Ctrl+C: sending signal %d to process %d...\n",
               foreground_signal, foreground_pid);
        kill(foreground_pid, foreground_signal);
        if (foreground_signal == SIGKILL) {
            foreground_pid = 0;  /* SIGKILL guarantees death */
        }
        /* For catchable signals (SIGINT), keep foreground_pid set —
         * the child may survive if it has a handler */
    } else {
        printf("[SHELL] You pressed Ctrl+C!\n");
    }
}
```

### What `kill(foreground_pid, foreground_signal)` Does

The `kill()` library function triggers syscall `SYS_kill` with arguments `(pid, foreground_signal)`.

The signal sent depends on `foreground_signal`:

- **SIGKILL (9)** — used by `test sigint` (child has no handler). The kernel takes the SIGKILL fast path: immediate `TSTATE_DEAD` + queue removal. Child is guaranteed dead.
- **SIGINT (2)** — used by `test sigint-custom` (child has a custom handler). The kernel marks SIGINT as pending via `tcb_add_pending_signal()`. When the child is next scheduled and traps, `deliver_signal()` redirects it to the child's own handler. See [test_sigint_custom.md](test_sigint_custom.md) for the full walkthrough.

In the kernel (`sys_kill` in TSyscall.c), SIGKILL has a **fast path**:

```c
void sys_kill(tf_t *tf)
{
    int pid = syscall_get_arg2(tf);     // Target PID (e.g., 7)
    int signum = syscall_get_arg3(tf);  // Signal number (9 = SIGKILL)

    if (signum == SIGKILL) {
        // Immediate termination — don't wait for signal delivery
        if (tcb_get_state(pid) == TSTATE_READY) {
            tqueue_remove(NUM_IDS, pid);  // Remove from ready queue
        }
        tcb_set_state(pid, TSTATE_DEAD);   // Mark as dead
        tcb_set_pending_signals(pid, 0);    // Clear all pending signals
        return;
    }

    // For non-SIGKILL: just mark as pending for later delivery
    tcb_add_pending_signal(pid, signum);
}
```

**After `sys_kill` returns:**
- Child (PID 7) has `state = TSTATE_DEAD` — it will never be scheduled again
- Child has been removed from the ready queue — `tqueue_dequeue` will never find it
- Effectively, the child ceases to exist

The handler then:
1. Checks `foreground_signal == SIGKILL`: if so, sets `foreground_pid = 0` — child is guaranteed dead
2. If `foreground_signal == SIGINT`: keeps `foreground_pid` set — child may survive if it has a handler
3. Returns via the C `ret` instruction → popping the return address from the stack

---

## Layer 9: Trampoline → sigreturn → Context Restoration

### Step 1: Handler Returns — CPU Jumps to Trampoline

When `sigint_handler()` executes `ret`:
- `ret` pops `[ESP]` into `EIP`
- `[ESP]` = `trampoline_addr` (set by `deliver_signal`)
- CPU begins executing the trampoline code **on the user stack**

### Step 2: Trampoline Executes

The trampoline is machine code written directly to the user stack by `deliver_signal`:

```nasm
; 12 bytes of x86 machine code:
B8 98 00 00 00    mov eax, 152        ; SYS_sigreturn = 152
CD 30             int 0x30            ; Trigger syscall trap
EB FE             jmp $               ; Infinite loop (never reached — safety)
90 90 90          nop nop nop         ; Padding to 12 bytes
```

The `int 0x30` instruction traps to the kernel. The trap handler dispatches to `sys_sigreturn` based on `EAX = 152`.

### Step 3: sys_sigreturn() — Kernel Restores Context

**File**: `kern/trap/TSyscall/TSyscall.c`

```c
void sys_sigreturn(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t saved_esp_addr, saved_eip_addr;
    uint32_t saved_esp, saved_eip;

    // 1. Get addresses where ESP/EIP were saved (from TCB)
    tcb_get_signal_context(cur_pid, &saved_esp_addr, &saved_eip_addr);

    // 2. Read the actual saved values from user stack
    pt_copyin(cur_pid, saved_esp_addr, &saved_esp, sizeof(uint32_t));
    pt_copyin(cur_pid, saved_eip_addr, &saved_eip, sizeof(uint32_t));

    // 3. Clear signal context in TCB
    tcb_clear_signal_context(cur_pid);

    // 4. Restore original trapframe
    tf->esp = saved_esp;   // Original stack pointer (before signal)
    tf->eip = saved_eip;   // Original instruction pointer (before signal)

    syscall_set_errno(tf, E_SUCC);
}
```

### Step 4: CPU Resumes at Original Location

`trap()` → `trap_return(tf)` → `iret`:
- `EIP` = original EIP (inside the `int 0x30` instruction of `sys_readline` inline assembly)
- `ESP` = original ESP (the shell's stack before signal delivery)

The shell is now back exactly where it was when `sys_readline` trapped to the kernel. The `int 0x30` instruction "returns" normally, and the shell's main loop continues:

```c
while(1) {
    buf[0] = '\0';                    // Already cleared
    if (shell_readline(buf) < 0) {    // Returns -1 (interrupted)
        continue;                      // Skip to next iteration
    }
    if (buf[0] != '\0') {            // buf[0] == '\0' from earlier clear
        if (runcmd(buf) < 0)
            break;
    }
}
```

The shell starts a new `readline()` → prints `">:"` prompt → ready for the next command.

---

## Complete Timing Analysis: What Happens When

This timeline shows every event in chronological order, with approximate timestamps based on CertiKOS's 5ms scheduling quantum:

```
T=0ms     Shell: user types "test sigint" + Enter
T=1ms     Shell: runcmd("test sigint") → shell_test_signal()
T=1.5ms   Shell: spawn(7, 1000) → kernel creates child PID 7
          Kernel: proc_create → parse ELF → setup page tables
          Kernel: child PID 7 added to ready queue (TSTATE_READY)
T=2ms     Shell: foreground_pid = 7; return 0;
T=2.5ms   Shell: main loop → buf[0] = '\0' → shell_readline(buf)
T=3ms     Shell: sys_readline(buf) → int 0x30 → kernel mode
          Kernel: sys_readline → readline(">:") → prints ">:"
          Kernel: readline → getchar() → cons_getc() → 0 (no key)
T=3.5ms   Kernel: thread_yield() [in getchar loop]
          Scheduler: shell → READY, child dequeued → RUN

T=3.5ms–  Child: printf("Hello World! (0)") → sys_puts → kernel → return
8.5ms     Child: printf("Hello World! (1)") → sys_puts → kernel → return
          ...continues printing...
T=8.5ms   Timer IRQ → sched_update() → SCHED_SLICE exceeded
          Scheduler: child → READY, shell dequeued → RUN
          Shell: resumes in getchar() → cons_getc() → 0 → thread_yield()
T=9ms     Child: continues printing (2), (3), ...

          [This alternation continues for seconds while user watches output]

T=5000ms  (User decides to press Ctrl+C)

T=5000ms  Hardware: Ctrl key press → scancode 0x1D → shift |= CTL
T=5001ms  Hardware: C key press → scancode 0x2E

          At some point during timesharing, the shell gets scheduled:
T=5002ms  Shell: getchar() → cons_getc() → keyboard_intr()
          kbd_proc_data(): data = 0x2E, shift has CTL → ctlmap[0x2E] = 3
          cons_intr: cons.buf[wpos++] = 3
          cons_getc: rpos != wpos → return 3
T=5002ms  getchar returns 0x03 to readline
T=5002ms  readline: c == 0x03 → putchar('^'), putchar('C'), putchar('\n')
          readline returns NULL

T=5002ms  sys_readline: kernbuf == NULL
          tcb_add_pending_signal(shell_pid, SIGINT) → bit 2 set
          set errno = E_INVAL_EVENT
          copy '\0' to user buf
          return

T=5002ms  trap(): kstack_switch(shell_pid)
          handle_pending_signals(tf):
            pending = 0x04 → signum = 2 → SIGINT
            clear bit → pending = 0x00
            handler registered? YES (sigint_handler)
            deliver_signal(tf, 2):
              save orig ESP/EIP on user stack
              write 12-byte trampoline
              push signum=2, ret addr=trampoline
              tf->eip = sigint_handler
              tf->esp = new_esp
          set_pdir_base(shell_pid)
          trap_return(tf) → iret

T=5003ms  USER MODE: sigint_handler(2) executes
          foreground_pid (7) > 0 → YES
          printf("[SHELL] Ctrl+C: terminating process 7...")
          kill(7, SIGKILL) → int 0x30 → sys_kill:
            SIGKILL fast path:
            state(7) == TSTATE_READY → tqueue_remove(64, 7)
            state(7) = TSTATE_DEAD
            pending_signals(7) = 0
          foreground_pid = 0
          sigint_handler returns (ret)

T=5003ms  ret → EIP = trampoline_addr (on stack)
          Trampoline: mov eax, 152; int 0x30
          Kernel: sys_sigreturn → restore ESP/EIP from stack
          trap_return → iret

T=5004ms  Shell: back at sys_readline return point
          shell_readline returns -1
          main loop: continue
          buf[0] = '\0'
          shell_readline(buf) → readline(">:") → prints ">:"
          Shell is at prompt, ready for next command

T=5004ms+ Normal shell operation continues
```

---

## The Scheduling Dance: Shell and Child Timesharing

Understanding how the shell and child interleave is critical for understanding when Ctrl+C can be detected.

### CertiKOS Scheduling Model

CertiKOS uses **cooperative + timer-based** round-robin scheduling:

| Mechanism | How It Works | When It Triggers |
|-----------|-------------|-----------------|
| `thread_yield()` | Explicitly called by running code | In `getchar()` poll loop, after `cons_getc()` returns 0 |
| `sched_update()` | Called by timer interrupt handler | Every timer tick; yields if `SCHED_SLICE` (5ms) exceeded |

Both call the same underlying scheduler:

```c
// kern/thread/PThread/PThread.c
void thread_yield(void)
{
    unsigned int old_cur_pid, new_cur_pid;
    old_cur_pid = get_curid();

    // Enqueue current process as READY
    tcb_set_state(old_cur_pid, TSTATE_READY);
    tqueue_enqueue(NUM_IDS, old_cur_pid);    // Add to tail of ready queue

    // Dequeue next process
    new_cur_pid = tqueue_dequeue(NUM_IDS);   // Remove from head of ready queue
    tcb_set_state(new_cur_pid, TSTATE_RUN);
    set_curid(new_cur_pid);

    // Context switch (saves/restores kernel context — registers, stack)
    kctx_switch(old_cur_pid, new_cur_pid);
}
```

### Ready Queue Structure

```c
// kern/thread/PTQueueIntro/PTQueueIntro.c
struct TQueue {
    unsigned int head;    // PID of first process (NUM_IDS = empty)
    unsigned int tail;    // PID of last process (NUM_IDS = empty)
};

// Queue for all CPUs; index NUM_IDS (64) is the ready queue
struct TQueue TQueuePool[NUM_CPUS][NUM_IDS + 1];
```

The queue uses the TCB's `prev`/`next` fields as linked list pointers:

```
Ready Queue (index 64):
head → PID 7 ←→ PID 2 ← tail
       next=2    prev=7
       prev=64   next=64

When thread_yield dequeues PID 7:
head → PID 2 ← tail
       prev=64
       next=64
PID 7 is now TSTATE_RUN (not in queue)
```

### Interleaving Pattern

```
Time ─────────────────────────────────────────────────────────────→

Shell:  [readline/getchar]     [getchar → yield]      [getchar → yield]
         poll → 0 → yield       poll → 0 → yield       poll → 0x03 → ^C!
              ↓                      ↓                      ↓
Sched:  yield(shell→child)   yield(child→shell)       yield(shell→child)
              ↓                      ↑                      ↑
Child:       [printf loop...]  [timer→yield]       [printf loop...]
         "Hello (0)"            back to shell       "Hello (5)"
         "Hello (1)"                                "Hello (6)"
         "Hello (2)" ← timer                        ...
```

**Key insight**: The shell can only detect Ctrl+C when it's **scheduled and executing** in `getchar()`. If the user presses Ctrl+C while the child is running, the characters (0x03) are buffered in the console ring buffer. When the shell next runs `getchar()` → `cons_getc()`, it reads the buffered 0x03.

The maximum latency from key press to detection is approximately:
- One `SCHED_SLICE` (5ms) for the child's remaining quantum to expire
- Plus one `thread_yield` cycle for the scheduler to switch to the shell
- Total: ~5–10ms — imperceptible to humans

---

## The Foreground Process Model

### CertiKOS Implementation

```c
/* In user/shell/shell.c */
static int foreground_pid = 0;        // 0 = no foreground process
static int foreground_signal = SIGKILL; // default signal to send on Ctrl+C
```

**Lifecycle:**

```
1. Shell starts:            foreground_pid = 0, foreground_signal = SIGKILL
2. "test sigint" command:   pid = spawn(7, 1000)
                            foreground_pid = pid  (e.g., 7)
                            foreground_signal = SIGKILL
3. "test sigint-custom":    pid = spawn(8, 1000)
                            foreground_pid = pid  (e.g., 7)
                            foreground_signal = SIGINT  (catchable!)
4. Ctrl+C handler:          kill(foreground_pid, foreground_signal)
                            if (SIGKILL) foreground_pid = 0
                            if (SIGINT)  foreground_pid stays set
5. Back to prompt:          foreground_pid = 0 (eventually)
```

> For the full walkthrough of `test sigint-custom` (where the child catches SIGINT), see [test_sigint_custom.md](test_sigint_custom.md).

### Comparison with Real UNIX Job Control

```
UNIX Model (simplified):
┌──────────────────────────────────────────────────────────────┐
│                     Terminal (tty)                            │
│  - Knows foreground process group (via tcsetpgrp)            │
│  - Detects Ctrl+C in line discipline                         │
│  - Sends SIGINT to ALL processes in foreground group         │
│  - Supports fg/bg/jobs/& for job control                     │
│  - Process groups with setpgid()                             │
│  - Sessions with setsid()                                    │
└──────────────────────────────────────────────────────────────┘

CertiKOS Model:
┌──────────────────────────────────────────────────────────────┐
│                     Shell Process                            │
│  - Tracks foreground_pid + foreground_signal variables        │
│  - Detects Ctrl+C via readline NULL return                   │
│  - Sends foreground_signal to ONE foreground process         │
│  - SIGKILL (test sigint) or SIGINT (test sigint-custom)      │
│  - No bg/fg/jobs/& — single foreground only                  │
│  - No process groups or sessions                             │
└──────────────────────────────────────────────────────────────┘
```

Despite the simplification, the user experience is identical: type a command, see output, press Ctrl+C, output stops, prompt returns.

---

## Signal Stack Frame: Byte-Level Layout

This is the precise memory layout created by `deliver_signal()` on the user's stack:

```
Address (relative)    Content                       Size    Purpose
───────────────────────────────────────────────────────────────────────────
orig_esp             [shell's original stack data]   —      Pre-existing stack

orig_esp - 4         saved_eip (4 bytes)             4      Original EIP for sigreturn
                     e.g., 0x40001A2F                       (return address in sys_readline)

orig_esp - 8         saved_esp (4 bytes)             4      Original ESP for sigreturn
                     e.g., 0x4FFFFF00

orig_esp - 20        Trampoline code (12 bytes):     12     Executable code on stack
                     B8 98 00 00 00  mov eax, 152
                     CD 30           int 0x30
                     EB FE           jmp $
                     90 90 90        nop nop nop

orig_esp - 24        Signal number: 0x02             4      Argument 1 to handler
                     (SIGINT = 2)

orig_esp - 28        Return address                  4      Points to trampoline_addr
                     (= orig_esp - 20)                      (= orig_esp - 20)

                     ← tf->esp (new ESP) points here
───────────────────────────────────────────────────────────────────────────
                     Total signal frame overhead: 28 bytes
```

### How the x86 cdecl Calling Convention Uses This

When `iret` jumps to `sigint_handler`:

```nasm
; x86 function prologue (generated by compiler):
sigint_handler:
    push ebp             ; Save old base pointer
    mov  ebp, esp        ; Set up new frame
    ; At this point:
    ;   [ebp+8]  = signum (2)     ← first argument
    ;   [ebp+4]  = return address (trampoline_addr)
    ;   [ebp+0]  = saved ebp
```

When the handler function `ret`s:

```nasm
; x86 function epilogue:
    leave                ; mov esp, ebp; pop ebp
    ret                  ; pop [esp] → EIP = trampoline_addr
                         ; CPU jumps to trampoline code on stack
```

---

## Three Bugs and Their Fixes

### Bug 1: Ready Queue Corruption

**Symptom**: After killing a process, the scheduler stopped working. No more context switches. System appeared to hang.

**Root Cause**: `tqueue_remove()` was called on a process with `TSTATE_RUN` — a process currently running on the CPU.

**The Ready Queue Implementation:**

```c
// kern/thread/PTQueueInit/PTQueueInit.c
void tqueue_remove(unsigned int chid, unsigned int pid) {
    unsigned int prev = tcb_get_prev(pid);
    unsigned int next = tcb_get_next(pid);

    // If this was the only element:
    if (prev == NUM_IDS && next == NUM_IDS) {
        tqueue_set_head(chid, NUM_IDS);  // Queue now empty
        tqueue_set_tail(chid, NUM_IDS);
    }
    // If this was the head:
    else if (prev == NUM_IDS) {
        tqueue_set_head(chid, next);
        tcb_set_prev(next, NUM_IDS);
    }
    // If this was the tail:
    else if (next == NUM_IDS) {
        tqueue_set_tail(chid, prev);
        tcb_set_next(prev, NUM_IDS);
    }
    // Middle element:
    else {
        tcb_set_next(prev, next);
        tcb_set_prev(next, prev);
    }
}
```

**The problem**: When a process is `TSTATE_RUN` (currently executing), it's NOT in the ready queue. Its `prev` and `next` are both `NUM_IDS` (sentinel value). Calling `tqueue_remove()` on it triggers the "only element" case:

```
Before remove (RUNNING process has prev=64, next=64):
  Ready Queue: head→PID 5←→PID 3←tail   (two processes waiting)
  Running: PID 7 (prev=64, next=64)      (not in queue!)

tqueue_remove(64, 7) triggers first branch:
  tqueue_set_head(64, 64)  → head = 64 (EMPTY!)
  tqueue_set_tail(64, 64)  → tail = 64 (EMPTY!)

After remove:
  Ready Queue: head=64, tail=64  (APPEARS EMPTY — but PID 5 and 3 are still linked!)
  PID 5 and 3 are now ORPHANED — lost from the queue forever
```

**The Fix**: State guard in `terminate_process()` and `sys_kill()`:

```c
if (tcb_get_state(pid) == TSTATE_READY) {
    tqueue_remove(NUM_IDS, pid);  // Only remove if actually in the queue
}
tcb_set_state(pid, TSTATE_DEAD);
```

### Bug 2: Child Process Starvation

**Symptom**: After `test sigint`, the child process never prints anything. The shell appears to hang waiting for input. Ctrl+C does nothing because the child never ran.

**Root Cause**: The original `getchar()` had no `thread_yield()`:

```c
// BROKEN VERSION:
int getchar(void) {
    int c;
    while ((c = cons_getc()) == 0)
        ;  // Spin-wait — CPU locked in kernel mode
    return c;
}
```

**Why this causes starvation:**

1. Shell calls `sys_readline()` → traps to kernel
2. `readline()` → `getchar()` → enters spin loop
3. `cons_getc()` returns 0 (no key pressed)
4. Loop iterates millions of times per second
5. Timer interrupt fires → `sched_update()` → checks tick count
6. But `sched_update()` in CertiKOS doesn't preempt the spin loop effectively because the current thread is still in a kernel code path
7. Child process never gets CPU time → it never executes → no output

**The Fix**: Add `thread_yield()` to the polling loop:

```c
// FIXED VERSION:
int getchar(void) {
    int c;
    while ((c = cons_getc()) == 0)
        thread_yield();  // Give other processes a chance to run
    return c;
}
```

Now each iteration:
1. Poll keyboard → no key → `cons_getc()` returns 0
2. `thread_yield()` → shell added to ready queue → child dequeued → child runs
3. Child prints output for one scheduling quantum (~5ms)
4. Timer → `sched_update()` → child yields → shell dequeued → shell resumes
5. Shell polls keyboard again → repeat

### Bug 3: Unknown Command After Ctrl+C

**Symptom**: After successfully terminating the foreground process with Ctrl+C, the shell printed:
```
Unknown command 'P'
try 'help' to see all supported commands.
```

**Root Cause**: Three interacting problems:

**Problem 1**: `sys_sigreturn()` only restores ESP and EIP — not EAX or other general-purpose registers.

**Problem 2**: The user-space `sys_readline()` wrapper reads `errno` from EAX:
```c
asm volatile("int %2"
    : "=a" (errno),     // EAX → errno after int 0x30 returns
      "=b" (ret)
    : ...);
return errno ? -1 : 0;  // If errno is 0 → returns 0 (success)
```

After `sigreturn`, EAX contains the value from the signal handler's last operation — not the `E_INVAL_EVENT` that `sys_readline` set. If EAX happened to be 0 (e.g., from `foreground_pid = 0`), then `errno = 0` → `sys_readline()` returns 0 (success).

**Problem 3**: The shell's `buf` contained stale data from before the syscall (possibly from a previous command). With `sys_readline()` returning success, the shell called `runcmd(buf)` with this stale data.

**The Fix**: Two defensive measures in the shell's main loop:

```c
while(1) {
    buf[0] = '\0';                   // 1. Clear buffer before readline
    if (shell_readline(buf) < 0) {
        continue;                     // Interrupted → retry
    }
    if (buf[0] != '\0') {           // 2. Only run if buffer has content
        if (runcmd(buf) < 0)
            break;
    }
}
```

1. `buf[0] = '\0'` — ensures the buffer starts empty every iteration
2. `buf[0] != '\0'` — guards against running commands on an empty or stale buffer

Even if `sys_readline` returns 0 (success) due to the register issue, `buf[0]` will be `'\0'` (because `sys_readline`'s kernel code copied an empty string to the user buffer on interruption), so `runcmd` is never called.

---

## Differences from Real UNIX

| Aspect | Linux/UNIX | CertiKOS |
|--------|-----------|----------|
| **Where Ctrl+C is detected** | Terminal line discipline (tty driver, `n_tty.c`) | `readline()` in `console.c` |
| **Who sends SIGINT** | tty driver to foreground process group | `sys_readline` to calling process |
| **Signal target** | All processes in foreground PGID | Single calling process (shell) |
| **How shell kills child** | Child receives SIGINT directly from tty | Shell catches SIGINT, manually `kill()`s child |
| **Character buffering** | Multi-layer: keyboard buffer, tty buffer, line discipline buffer | Single 512-byte ring buffer in `cons` struct |
| **I/O model** | Interrupt-driven with wait queues | Polled with `thread_yield()` |
| **Signal stack frame** | Full `struct sigframe`: all regs, FPU state, signal mask, restorer | Manual: ESP + EIP only (28 bytes total) |
| **`sigreturn`** | Restores all registers, FPU, signal mask via `__NR_rt_sigreturn` | Restores ESP and EIP only |
| **Ctrl+C character echo** | Configurable via `stty echo` / `termios` | Hardcoded in `readline()` |
| **Process groups** | Full PGID/session/tty model (`setpgid`, `tcsetpgrp`) | Single `foreground_pid` variable |
| **Job control** | `bg`, `fg`, `jobs`, `&`, `Ctrl+Z` (SIGTSTP) | None |
| **`getchar` blocking** | `read()` syscall sleeps on wait queue, woken by IRQ | `cons_getc()` polls + `thread_yield()` |
| **SIGKILL delivery** | Marked pending; delivered at next return-to-user | `sys_kill` fast path: immediate state change |

### What CertiKOS Gets Right

Despite the simplifications, these fundamental concepts are faithfully implemented:
1. **Hardware → character translation**: Scancode tables, modifier tracking, control character generation
2. **Signal lifecycle**: Register handler → mark pending → deliver (rewrite trapframe) → execute handler → return (trampoline + sigreturn)
3. **Asynchronous signal delivery**: Signal is delivered at trap boundary, not at the point of generation
4. **Signal isolation**: SIGINT goes to the shell, not the child — matches the design principle of "process-directed signals"
5. **Uncatchable SIGKILL**: No handler, immediate termination (matching POSIX semantics)

---

## Complete File Inventory

### Files Created

| File | Lines | Purpose |
|------|-------|---------|
| `user/sigint_test/sigint_test.c` | ~15 | Infinite "Hello World!" printing loop |
| `user/sigint_test/Makefile.inc` | ~15 | Build rules for sigint_test binary |
| `user/sigint_custom_test/sigint_custom_test.c` | ~33 | Catches SIGINT with custom handler — prints "YOU CAN'T KILL ME!!" |
| `user/sigint_custom_test/Makefile.inc` | ~28 | Build rules for sigint_custom_test binary |

### Files Modified

| File | Section | Change |
|------|---------|--------|
| `kern/dev/console.c` | `getchar()` | Added `extern void thread_yield(void);` declaration and `thread_yield()` call in poll loop |
| `kern/dev/console.c` | `readline()` | Added `c == 0x03` check → print `^C\n`, return NULL |
| `kern/trap/TSyscall/TSyscall.c` | `sys_readline()` | Added NULL check → `tcb_add_pending_signal(SIGINT)`, error return, empty string copy |
| `kern/trap/TSyscall/TSyscall.c` | `sys_kill()` | Added SIGKILL fast path with `TSTATE_READY` guard |
| `kern/trap/TSyscall/TSyscall.c` | `sys_spawn()` | Added `elf_id == 7` → `sigint_test` mapping, `elf_id == 8` → `sigint_custom_test` mapping |
| `kern/trap/TTrapHandler/TTrapHandler.c` | `handle_pending_signals()` | Existing — processes SIGINT (calls `deliver_signal` when handler registered) |
| `kern/trap/TTrapHandler/TTrapHandler.c` | `terminate_process()` | Added `TSTATE_READY` guard before `tqueue_remove()` |
| `user/shell/shell.c` | globals | Added `foreground_pid` and `foreground_signal` variables |
| `user/shell/shell.c` | `sigint_handler()` | Created — sends `foreground_signal` to foreground child (SIGKILL or SIGINT) |
| `user/shell/shell.c` | `main()` | Added `sigaction(SIGINT, ...)` registration, `buf[0]='\0'` clear, `buf[0]!='\0'` guard |
| `user/shell/shell.c` | `shell_test_signal()` | Added `test sigint` command (spawns child, sets `foreground_pid`, `foreground_signal = SIGKILL`) and `test sigint-custom` command (spawns child with custom handler, `foreground_signal = SIGINT`) |
| `user/Makefile.inc` | | Added sigint_test include and target |

### Files Referenced (Not Modified)

| File | Why |
|------|-----|
| `kern/dev/keyboard.c` | Scancode tables (`shiftcode`, `ctlmap`, `charcode`), `kbd_proc_data()`, `keyboard_intr()` |
| `kern/dev/keyboard.h` | Key definitions (`KEY_UP`, `KEY_DN`, etc.) |
| `kern/dev/console.h` | `CONSOLE_BUFFER_SIZE` definition |
| `kern/lib/signal.h` | Signal definitions (`SIGINT`, `SIGKILL`, `NSIG`), `sig_state` struct |
| `kern/lib/thread.h` | Thread states (`TSTATE_READY`, `TSTATE_RUN`, etc.), `SCHED_SLICE` |
| `kern/thread/PThread/PThread.c` | `thread_yield()`, `thread_exit()`, `sched_update()` |
| `kern/thread/PTCBIntro/PTCBIntro.c` | TCBPool, signal accessor functions |
| `kern/thread/PTQueueInit/PTQueueInit.c` | `tqueue_enqueue()`, `tqueue_dequeue()`, `tqueue_remove()` |
| `user/include/syscall.h` | User-space syscall wrappers (inline assembly) |

---

## All Constants and Definitions

| Name | Value | File | Purpose |
|------|-------|------|---------|
| `SIGINT` | 2 | `kern/lib/signal.h` | Interrupt signal (Ctrl+C) |
| `SIGKILL` | 9 | `kern/lib/signal.h` | Uncatchable termination signal |
| `SIGSEGV` | 11 | `kern/lib/signal.h` | Segmentation fault signal |
| `NSIG` | 32 | `kern/lib/signal.h` | Number of signals (one per bitmask bit) |
| `SYS_sigreturn` | 152 | `kern/lib/syscall.h` | Syscall number for signal context restore |
| `SYS_readline` | — | `kern/lib/syscall.h` | Syscall number for reading a line |
| `SYS_kill` | — | `kern/lib/syscall.h` | Syscall number for sending signals |
| `T_SYSCALL` | 0x30 | `kern/lib/trap.h` | x86 trap number for syscalls |
| `0x03` | 3 | ASCII | ETX (End of Text) = Ctrl+C |
| `KBSTATP` | 0x64 | `kern/dev/keyboard.h` | Keyboard status I/O port |
| `KBDATAP` | 0x60 | `kern/dev/keyboard.h` | Keyboard data I/O port |
| `KBS_DIB` | 0x01 | `kern/dev/keyboard.h` | Data-In-Buffer status bit |
| `CTL` | 2 (1<<1) | `kern/dev/keyboard.c` | Control key modifier flag |
| `SHIFT` | 1 (1<<0) | `kern/dev/keyboard.c` | Shift key modifier flag |
| `C(x)` | `x - '@'` | `kern/dev/keyboard.c` | Macro: letter → control character |
| `CONSOLE_BUFFER_SIZE` | 512 | `kern/dev/console.h` | Ring buffer capacity in bytes |
| `BUFLEN` | 1024 | `kern/dev/console.c` | Maximum readline line length |
| `TSTATE_READY` | 0 | `kern/lib/thread.h` | Thread is in the ready queue |
| `TSTATE_RUN` | 1 | `kern/lib/thread.h` | Thread is currently executing on CPU |
| `TSTATE_SLEEP` | 2 | `kern/lib/thread.h` | Thread is sleeping on a channel |
| `TSTATE_DEAD` | 3 | `kern/lib/thread.h` | Thread is terminated — will not run again |
| `SCHED_SLICE` | 5 | `kern/lib/thread.h` | Scheduling quantum in milliseconds |
| `NUM_IDS` | 64 | config | Max process count; queue-empty sentinel |
| `E_SUCC` | 0 | `kern/lib/syscall.h` | Success error code |
| `E_INVAL_EVENT` | — | `kern/lib/syscall.h` | Error: invalid/interrupted event |
| `foreground_pid` | variable | `user/shell/shell.c` | Tracks current foreground child PID |
| `foreground_signal` | variable | `user/shell/shell.c` | Signal to send on Ctrl+C (SIGKILL or SIGINT) |
| `elf_id 7` | — | `TSyscall.c` | Identifier for sigint_test binary |
| `elf_id 8` | — | `TSyscall.c` | Identifier for sigint_custom_test binary |
