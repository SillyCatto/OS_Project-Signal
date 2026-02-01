# POSIX-Like Signal System Implementation in mCertiKOS

This document explains the implementation of a POSIX-like signal system in the mCertiKOS operating system, following the requirements specified in the project instructions.

## Overview

The implementation adds a minimal signal system that allows for inter-process communication and notification through signals, similar to UNIX/POSIX systems. The key features include:

- Signal registration using `sigaction()`
- Signal delivery via the `kill` system call
- User-defined signal handlers
- Shell commands for testing and utilizing signals

## Implementation Details

### 1. Signal Infrastructure in Kernel

#### `kern/lib/signal.h`

This file defines the core signal system data structures, including:

- `NSIG` (32): Maximum number of signals supported
- `sighandler_t`: Function pointer type for signal handlers
- `struct sigaction`: Structure for configuring signal behavior
- `struct sig_state`: Per-thread signal state, including:
  - Array of signal actions (`sigactions`)
  - Pending signals bitfield
  - Signal block mask

The signal numbers and flags follow POSIX conventions, with standard signals like `SIGINT`, `SIGKILL`, etc.

#### `kern/lib/thread.h`

The thread structure was modified to include the signal state:

```c
struct thread {
    // ...existing fields...
    struct sig_state sigstate;  // Signal state
};
```

This allows each thread to have its own set of signal handlers and pending signals.

### 2. Kernel Syscalls

#### `kern/trap/TSyscall/TSyscall.c`

Three system calls were implemented:

1. `sys_sigaction`: Registers or examines signal handlers
   - Validates the signal number
   - Stores the old handler if requested
   - Sets the new handler if provided

2. `sys_kill`: Sends a signal to a process
   - Validates the signal number and target process
   - Sets the signal as pending in the target process
   - Wakes up the process if it's sleeping

3. `sys_pause`: Blocks until a signal is delivered
   - Checks if any signals are already pending
   - If not, puts the process to sleep

#### `kern/lib/syscall.h` and `syscall.c`

The syscall interface was updated to include the new signal-related system calls:
- Added syscall numbers (`SYS_sigaction`, `SYS_kill`, `SYS_pause`)
- Added wrapper functions for user space

### 3. Signal Delivery Mechanism

#### `kern/trap/TTrapHandler/TTrapHandler.c`

The signal delivery mechanism was implemented in the trap handler:

1. `deliver_signal`: Handles the actual signal delivery
   - Retrieves the signal handler from the thread's signal state
   - Saves the current context
   - Sets up the argument (signal number) in the eax register
   - Changes the instruction pointer to the handler address

2. `trap_return`: Checks for pending signals before returning to user mode
   - Iterates through pending signals that aren't blocked
   - Clears the pending signal
   - Calls `deliver_signal` to deliver the signal
   - Returns to user space

Proper checks were added to ensure signals don't interrupt kernel-running threads and that blocked signals aren't delivered.

### 4. User Space Integration

#### `user/include/signal.h`

A user-space header for signals was created, mirroring the kernel definitions but exposing only what's needed for user applications:
- Signal number constants
- Signal flag constants
- The `sigaction` structure
- Function declarations for syscall wrappers

#### `user/shell/shell.c`

Two new shell commands were implemented:

1. `kill`: Sends a signal to a process
   ```
   kill <signal> <pid>
   ```
   - Parses the signal number and target PID
   - Calls the `kill()` syscall

2. `trap`: Registers a signal handler
   ```
   trap <signum> <handler>
   ```
   - Parses the signal number and handler address
   - Sets up a `sigaction` structure
   - Calls the `sigaction()` syscall

A `signal_handler` function was also implemented to demonstrate signal handling by printing a message when signals are received.

### 5. Test Cases

Several test cases were added to `shell.c` in the `shell_test()` function:

1. Basic signal handling:
   - Registers a handler for `SIGUSR1`
   - Sends `SIGUSR1` to self
   - Verifies the handler was called by printing a message

2. Signal blocking:
   - Registers a handler for `SIGUSR2` with `SIGUSR2` blocked in the mask
   - Sends `SIGUSR2` to self
   - Demonstrates that the signal is not immediately delivered

3. `pause()` functionality:
   - Calls `pause()` to block
   - Wakes up when a signal is delivered
   - Confirms resumption by printing a message

The tests can be toggled using the existing shell mode variable.

## Workflow and Limitations

### Workflow

The signal system operates as follows:

1. **Registration**: A process registers a handler for a specific signal using `sigaction()`.
2. **Signal Sending**: Another process (or the same one) sends a signal using `kill()`.
3. **Signal Pending**: The kernel marks the signal as pending in the target process.
4. **Signal Delivery**: When the target process returns to user mode, the kernel checks for pending signals.
5. **Handler Invocation**: If a signal is pending and not blocked, the kernel diverts execution to the handler.
6. **Return**: After handling the signal, the process continues normal execution.

### Limitations

The implementation has several limitations:

1. **No Real-time Signals**: Only basic signal functionality is supported, without real-time signals.
2. **Simplified Masking**: Signal blocking is implemented but not with full POSIX semantics.
3. **No Signal Queuing**: If multiple instances of the same signal are sent, only one will be delivered.
4. **Limited Signal Information**: The `siginfo_t` structure is simplified as `void*`.
5. **No Signal Stack**: Alternative signal stacks are not supported.
6. **No Signal Inheritance**: Signal handlers and masks are not inherited across process creation.

## Conclusion

This implementation provides a minimal but functional POSIX-like signal system in mCertiKOS. It demonstrates key concepts of signal handling while maintaining simplicity for educational purposes. The implementation can be expanded in the future to support more advanced features such as real-time signals, complete signal masking, and signal inheritance. 