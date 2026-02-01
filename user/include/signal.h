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