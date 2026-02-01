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
