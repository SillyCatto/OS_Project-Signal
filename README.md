# mCertiKOS — POSIX Signal Implementation

5th semester OS course lab project: a POSIX-compliant signal delivery engine built into [mCertiKOS](https://flint.cs.yale.edu/certikos/), an educational x86 microkernel.

## What Was Implemented

- **Signal delivery pipeline** — pending bitmask per process, kernel-side dispatch on every trap return, userspace trampoline for `sigreturn`
- **`sigaction` / `kill` / `sigreturn` syscalls** — register handlers, send signals, restore context
- **SIGKILL (9)** — unconditional process termination, not catchable
- **SIGSEGV (11)** — graceful handling of page faults (e.g. NULL dereference) instead of kernel panic
- **SIGINT (2)** — Ctrl+C delivery from shell to foreground process, with both default-kill and custom-handler modes
- **Shell integration** — `kill`, `trap`, `spawn`, and `test` commands; Ctrl+C forwarding to foreground process

## Build & Run

**Prerequisites:** GCC cross-compiler for i386, QEMU, Python 3, GNU Make.

```bash
# Build everything
make

# Launch in QEMU (graphical)
make qemu

# Launch in QEMU (terminal-only, exit with Ctrl-a x)
make qemu-nox
```

Once the OS boots you'll land in the mCertiKOS shell (`>:`).

## Testing Signals

### 1. `trap` — Register a Signal Handler on the Shell

```
>: trap 2
```

Registers a generic handler for signal 2 (SIGINT) on the **shell process itself**. Sending that signal to the shell will print `*** Received signal 2 ***` instead of killing it.

### 2. SIGKILL — Force Kill a Process

```
>: spawn 1            # spawns "ping" process, note the PID (e.g. 7)
>: kill -9 7          # sends SIGKILL to PID 7 → process terminated
>: kill -9 7          # send again → "Failed to send signal" (confirms PID 7 is dead)
```

### 3. SIGSEGV — Graceful Segfault Handling

```
>: test sigsegv
```

Spawns a process that dereferences a NULL pointer. Instead of a kernel panic, the OS delivers SIGSEGV and terminates the faulting process. The shell stays alive.

### 4. SIGINT — Ctrl+C Default (Kill)

```
>: test sigint
```

Spawns a process that prints continuously. Press **Ctrl+C** — the shell sends SIGKILL to the foreground process, terminating it immediately (no handler installed).

### 5. SIGINT — Ctrl+C Custom Handler

```
>: test sigint-custom
```

Spawns a process with a custom SIGINT handler. Press **Ctrl+C** — the shell sends SIGINT (not SIGKILL). The process **catches** it and prints `"YOU CAN'T KILL ME!!"` instead of dying. Press Ctrl+C multiple times to see repeated catches. Use `kill -9 <pid>` to force-kill it.
