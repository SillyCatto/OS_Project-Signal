#include <proc.h>
#include <stdio.h>
#include <syscall.h>
#include <x86.h>

int main(int argc, char **argv)
{
    printf("[sigint_test] Process started. Printing continuously...\n");
    printf("[sigint_test] Press Ctrl+C to terminate.\n");

    int count = 0;
    while (1) {
        printf("[sigint_test] Hello World! (%d)\n", count++);
    }

    /* Should never reach here */
    printf("[sigint_test] ERROR: Should not reach this line!\n");
    return 0;
}
