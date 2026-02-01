# High level plan to implement POSIX compliant signal mechanism in mCertikOS

### Project instructions (given by course instructor)

```
Signal is a POSIX inter-process communication (IPC) functionality that allows a user to register several user-level call-back functions, and when the kernel needs to notify the user that some events occur, it could select one of the call-back functions to let the user run it.

A typical signal using scenario is the shell command kill. When user issue command:

kill -num pid

to the kernel, the thread #pid will receive a signal numbered by #num.
When a user thread start, it may use syscall sigaction() to register a call-back function with the specified signal number for preparing to receive the upcoming signals:

struct sigaction {
  void (*sa_handler)(int);
  void (*sa_sigaction)(int, siginfo_t *, void *);
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};

Alternatively, a user thread may use syscall pause() to sleep until a signal is delivered. A full list of signal related syscalls could be found with man 7 signal.

In this project, you are required to implement a set of syscalls that is similar to the POSIX signal and show that a user thread could be properly killed or notified with the signal mechanism. In addition, you need to equip the shell in lab5 with additional commands, such as kill and trap to demonstrate your signal implementation.

References:

[1] H. Arora, “Linux signals fundamentals - Part I and II”, https://www.thegeekstuff.com/2012/03/linux-signals-fundamentals/, 2012.

```

## My Goal

In my University OS course, I am given a project to study the POSIX signal mechanism thoroughly and implement a POSIX compliant signal mechanism in this mcertikos, this current folder (whole workspace) contains the solution to the project instructions (the implementation of the project). So what I want to do is to understand the implementation thoroughly, the theory, what is happening, how its working etc.

## Instructions

- Right now, im studying the codebase to get a good understanding of the implementation of the signal mechanism
- You are an expert and professional OS engineer and developer, your job is to inspect and thoroughly study the codebase and explain the POSIX compliant signal mechanism implementation to me in details: what is signal, difference between interrupt vs signal vs trap, how signals work, how is it sent to a target process at a low level, how custom signal handlers are registered to kernel and how is it executed, how does switiching from user to kernel and back works, how program counter is changed by the kernel to execute the handler, how hijacking happens, how execution is resumed from where it was interrupted and all other details
- the author of the solution wrote a brief explanation at root, ./explanation.md, you may read it to get clear context
- right now, you are NOT allowed to modify any code file in any folder except for the ./docs folder, this is the folder where you will create necessary and relevant explanations and documentations in markdown files in details and you may also include nice, detailed but accurate diagrams in mermaid to provide more clear and easier visual understanding of the mechanisms where necessary, dont modify any other file in any other folder unless explicitly told to do so.
- at first, I plan to implement not all but a few common and easier POSIX signal types: SIGKILL, SIGINT, SIGSEGV, SIGALRM


## Notice

Currently, Im having error trying to compile the os code by running: make
the error is given below:

```
kern/trap/TSyscall/TSyscall.c: In function ‘sys_sigaction’:
kern/trap/TSyscall/TSyscall.c:375:41: warning: implicit declaration of function ‘tcb_get_entry’; did you mean ‘tcb_get_next’? [-Wimplicit-function-declaration]
  375 |     struct TCB *cur_tcb = (struct TCB *)tcb_get_entry(get_curid());
      |                                         ^~~~~~~~~~~~~
      |                                         tcb_get_next
kern/trap/TSyscall/TSyscall.c:385:26: error: invalid use of undefined type ‘struct TCB’
  385 |         *oldact = cur_tcb->sigstate.sigactions[signum];
      |                          ^~
kern/trap/TSyscall/TSyscall.c:390:16: error: invalid use of undefined type ‘struct TCB’
  390 |         cur_tcb->sigstate.sigactions[signum] = *act;
      |                ^~
kern/trap/TSyscall/TSyscall.c: In function ‘sys_kill’:
kern/trap/TSyscall/TSyscall.c:415:15: error: invalid use of undefined type ‘struct TCB’
  415 |     target_tcb->sigstate.pending_signals |= (1 << signum);
      |               ^~
kern/trap/TSyscall/TSyscall.c:418:19: error: invalid use of undefined type ‘struct TCB’
  418 |     if (target_tcb->state == TSTATE_SLEEP) {
      |                   ^~
kern/trap/TSyscall/TSyscall.c:418:30: error: ‘TSTATE_SLEEP’ undeclared (first use in this function)
  418 |     if (target_tcb->state == TSTATE_SLEEP) {
      |                              ^~~~~~~~~~~~
kern/trap/TSyscall/TSyscall.c:418:30: note: each undeclared identifier is reported only once for each function it appears in
kern/trap/TSyscall/TSyscall.c: In function ‘sys_pause’:
kern/trap/TSyscall/TSyscall.c:430:16: error: invalid use of undefined type ‘struct TCB’
  430 |     if (cur_tcb->sigstate.pending_signals != 0) {
      |                ^~
kern/trap/TSyscall/TSyscall.c:436:12: error: invalid use of undefined type ‘struct TCB’
  436 |     cur_tcb->state = TSTATE_SLEEP;
      |            ^~
kern/trap/TSyscall/TSyscall.c:436:22: error: ‘TSTATE_SLEEP’ undeclared (first use in this function)
  436 |     cur_tcb->state = TSTATE_SLEEP;
      |                      ^~~~~~~~~~~~
make: *** [kern/trap/TSyscall/Makefile.inc:10: obj/kern/trap/TSyscall/TSyscall.o] Error 1
```

probably its just missing some imports, make a file fix.md to fix the compilation and provide details on how to build and test the solution implementation to ensure its working

