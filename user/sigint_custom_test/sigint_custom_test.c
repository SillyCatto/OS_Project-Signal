#include <proc.h>
#include <stdio.h>
#include <syscall.h>
#include <x86.h>

#include "signal.h"

void my_sigint_handler(int signum)
{
    printf("\nYOU CAN'T KILL ME!!\n");
}

int main(int argc, char **argv)
{
    /* Register custom SIGINT handler */
    struct sigaction sa;
    sa.sa_handler = my_sigint_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    sigaction(SIGINT, &sa, 0);

    printf("[sigint_custom] Process started with custom SIGINT handler.\n");

    while (1) {
        printf(".");
    }

    /* Should never reach here */
    return 0;
}
