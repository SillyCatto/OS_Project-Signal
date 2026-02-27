#include <proc.h>
#include <stdio.h>
#include <syscall.h>
#include <x86.h>

int main(int argc, char **argv)
{
    printf("[sigsegv_test] Process started.\n");
    printf("[sigsegv_test] Attempting to dereference NULL pointer...\n");

    /* Intentionally trigger a segmentation fault by writing to address 0 */
    volatile int *bad_ptr = (volatile int *)0x0;
    *bad_ptr = 42;

    /* Should never reach here */
    printf("[sigsegv_test] ERROR: Should not reach this line!\n");
    return 0;
}
