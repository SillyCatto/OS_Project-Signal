#include <stdio.h>
#include <syscall.h>
#include "signal.h"
#include <types.h>

void sigint_handler(int signum)
{
    printf("Received SIGINT (%d)\n", signum);
}

void sigusr1_handler(int signum)
{
    printf("Received SIGUSR1 (%d)\n", signum);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int pid = getpid();
    
    printf("Signal test program (PID: %d)\n", pid);
    
    // Register SIGINT handler
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        printf("Failed to register SIGINT handler\n");
        return -1;
    }
    
    // Register SIGUSR1 handler
    sa.sa_handler = sigusr1_handler;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("Failed to register SIGUSR1 handler\n");
        return -1;
    }
    
    printf("Handlers registered. Waiting for signals...\n");
    
    while (1) {
        pause();  // Wait for signals
    }
    
    return 0;
} 