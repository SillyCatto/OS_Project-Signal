#include <lib/string.h>
#include <lib/trap.h>
#include <lib/syscall.h>
#include <lib/debug.h>
#include <lib/x86.h>
#include <lib/thread.h>
#include <lib/pmap.h>
#include <dev/intr.h>
#include <pcpu/PCPUIntro/export.h>

#include <vmm/MPTOp/export.h>
#include <thread/PThread/export.h>
#include <thread/PTCBIntro/export.h>
#include <thread/PCurID/export.h>
#include <thread/PTQueueInit/export.h>
#include <lib/signal.h>

#include "import.h"

void ide_intr(void);

static void trap_dump(tf_t *tf)
{
	if (tf == NULL)
		return;

	uintptr_t base = (uintptr_t) tf;

	KERN_DEBUG("trapframe at %x\n", base);
	KERN_DEBUG("\t%08x:\tedi:   \t\t%08x\n", &tf->regs.edi, tf->regs.edi);
	KERN_DEBUG("\t%08x:\tesi:   \t\t%08x\n", &tf->regs.esi, tf->regs.esi);
	KERN_DEBUG("\t%08x:\tebp:   \t\t%08x\n", &tf->regs.ebp, tf->regs.ebp);
	KERN_DEBUG("\t%08x:\tesp:   \t\t%08x\n", &tf->regs.oesp, tf->regs.oesp);
	KERN_DEBUG("\t%08x:\tebx:   \t\t%08x\n", &tf->regs.ebx, tf->regs.ebx);
	KERN_DEBUG("\t%08x:\tedx:   \t\t%08x\n", &tf->regs.edx, tf->regs.edx);
	KERN_DEBUG("\t%08x:\tecx:   \t\t%08x\n", &tf->regs.ecx, tf->regs.ecx);
	KERN_DEBUG("\t%08x:\teax:   \t\t%08x\n", &tf->regs.eax, tf->regs.eax);
	KERN_DEBUG("\t%08x:\tes:    \t\t%08x\n", &tf->es, tf->es);
	KERN_DEBUG("\t%08x:\tds:    \t\t%08x\n", &tf->ds, tf->ds);
	KERN_DEBUG("\t%08x:\ttrapno:\t\t%08x\n", &tf->trapno, tf->trapno);
	KERN_DEBUG("\t%08x:\terr:   \t\t%08x\n", &tf->err, tf->err);
	KERN_DEBUG("\t%08x:\teip:   \t\t%08x\n", &tf->eip, tf->eip);
	KERN_DEBUG("\t%08x:\tcs:    \t\t%08x\n", &tf->cs, tf->cs);
	KERN_DEBUG("\t%08x:\teflags:\t\t%08x\n", &tf->eflags, tf->eflags);
	KERN_DEBUG("\t%08x:\tesp:   \t\t%08x\n", &tf->esp, tf->esp);
	KERN_DEBUG("\t%08x:\tss:    \t\t%08x\n", &tf->ss, tf->ss);
}

void default_exception_handler(tf_t *tf)
{
	unsigned int cur_pid;

	cur_pid = get_curid();
	trap_dump(tf);

	KERN_PANIC("Trap %d @ 0x%08x.\n", tf -> trapno, tf -> eip);
}

void pgflt_handler(tf_t *tf)
{
	unsigned int cur_pid;
	unsigned int errno;
	unsigned int fault_va;

	cur_pid = get_curid();
	errno = tf -> err;
	fault_va = rcr2();

  //Uncomment this line if you need to see the information of the sequence of page faults occured.
	//KERN_DEBUG("Page fault: VA 0x%08x, errno 0x%08x, process %d, EIP 0x%08x.\n", fault_va, errno, cur_pid, tf -> eip);

	if (errno & PFE_PR) {
		trap_dump(tf);
		KERN_PANIC("Permission denied: va = 0x%08x, errno = 0x%08x.\n", fault_va, errno);
		return;
	}

	if (alloc_page(cur_pid, fault_va, PTE_W | PTE_U | PTE_P) == MagicNumber)
    KERN_PANIC("Page allocation failed: va = 0x%08x, errno = 0x%08x.\n", fault_va, errno);

}

/**
 * We currently only handle the page fault exception.
 * All other exceptions should be routed to the default exception handler.
 */
void exception_handler(tf_t *tf)
{
	unsigned int cur_pid;
	unsigned int trapno;

	cur_pid = get_curid();
	trapno = tf -> trapno;

	if (trapno == T_PGFLT)
		pgflt_handler(tf);
	else
		default_exception_handler(tf);
}



static int spurious_intr_handler (void)
{
    return 0;
}

static int timer_intr_handler (void)
{
    intr_eoi ();
    sched_update();
    return 0;
}

static int default_intr_handler (void)
{
    intr_eoi ();
    return 0;
}

/**
 * Any interrupt request except the spurious or timer should be
 * routed to the default interrupt handler.
 */
void interrupt_handler (tf_t *tf)
{
    unsigned int cur_pid;
    unsigned int trapno;

    cur_pid = get_curid ();

    trapno = tf -> trapno;

    switch (trapno)
    {
      case T_IRQ0 + IRQ_SPURIOUS:
          spurious_intr_handler ();
          break;
      case T_IRQ0 + IRQ_TIMER:
          timer_intr_handler ();
          break;
      // TODO: handle the disk interrupts here
      case T_IRQ0 + IRQ_IDE1:
          ide_intr();
          intr_eoi();
          break;
      case T_IRQ0 + IRQ_IDE2:
          break;
      default:
          default_intr_handler ();
    }
}

// Signal trampoline code to be placed on user stack
// This executes: mov eax, SYS_sigreturn; int 0x30; (infinite loop as safety)
// Machine code: B8 <syscall_num> 00 00 00  CD 30  EB FE

static void deliver_signal(tf_t *tf, int signum)
{
    unsigned int cur_pid = get_curid();
    struct sigaction *sa = tcb_get_sigaction(cur_pid, signum);

    KERN_INFO("[SIGNAL] deliver_signal: pid=%d signum=%d sa=%x\n", cur_pid, signum, (unsigned int)sa);

    if (sa != NULL && sa->sa_handler != NULL) {
        KERN_INFO("[SIGNAL] deliver_signal: sa->sa_handler=%x\n", (unsigned int)sa->sa_handler);

        // Save original context for sigreturn
        // Stack layout (growing down):
        //   [original esp area]
        //   saved_eip        <- for sigreturn to restore
        //   saved_esp        <- for sigreturn to restore
        //   trampoline code  <- 8 bytes: mov eax, SYS_sigreturn; int 0x30; jmp $
        //   signum           <- argument to handler
        //   trampoline_addr  <- return address (points to trampoline)
        //   [new esp]

        uint32_t orig_esp = tf->esp;
        uint32_t orig_eip = tf->eip;
        uint32_t new_esp = orig_esp;
        size_t copied;

        // Push saved_eip (for sigreturn)
        new_esp -= 4;
        copied = pt_copyout(&orig_eip, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_eip_addr = new_esp;

        // Push saved_esp (for sigreturn)
        new_esp -= 4;
        copied = pt_copyout(&orig_esp, cur_pid, new_esp, sizeof(uint32_t));
        uint32_t saved_esp_addr = new_esp;

        // Push trampoline code (8 bytes)
        // mov eax, SYS_sigreturn (B8 98 00 00 00) - 5 bytes
        // int 0x30 (CD 30) - 2 bytes
        // jmp $ (EB FE) - 2 bytes (safety infinite loop)
        uint8_t trampoline[12] = {
            0xB8, SYS_sigreturn, 0x00, 0x00, 0x00,  // mov eax, SYS_sigreturn
            0xCD, 0x30,                              // int 0x30
            0xEB, 0xFE,                              // jmp $ (infinite loop - safety)
            0x90, 0x90, 0x90                         // nop padding
        };
        new_esp -= 12;
        copied = pt_copyout(trampoline, cur_pid, new_esp, 12);
        uint32_t trampoline_addr = new_esp;

        // Align stack to 4 bytes if needed
        new_esp = new_esp & ~3;

        // Push signal number (argument)
        new_esp -= 4;
        uint32_t sig_arg = signum;
        copied = pt_copyout(&sig_arg, cur_pid, new_esp, sizeof(uint32_t));

        // Push return address (trampoline)
        new_esp -= 4;
        copied = pt_copyout(&trampoline_addr, cur_pid, new_esp, sizeof(uint32_t));

        KERN_INFO("[SIGNAL] Stack setup: orig_esp=%x orig_eip=%x tramp=%x new_esp=%x\n",
                  orig_esp, orig_eip, trampoline_addr, new_esp);

        // Update trapframe
        tf->esp = new_esp;
        tf->eip = (uint32_t)sa->sa_handler;

        // Store saved context location in TCB for sigreturn
        tcb_set_signal_context(cur_pid, saved_esp_addr, saved_eip_addr);

        KERN_INFO("[SIGNAL] Delivering signal %d to process %d, handler at %x\n",
                  signum, cur_pid, sa->sa_handler);
    } else {
        KERN_INFO("[SIGNAL] No handler for signal %d in process %d\n", signum, cur_pid);
    }
}

static void terminate_process(unsigned int pid)
{
    KERN_INFO("[SIGNAL] Terminating process %d (SIGKILL)\n", pid);

    // Set state to DEAD
    tcb_set_state(pid, TSTATE_DEAD);

    // Remove from ready queue (NUM_IDS is the ready queue)
    tqueue_remove(NUM_IDS, pid);

    // Clear any pending signals
    tcb_set_pending_signals(pid, 0);

    KERN_INFO("[SIGNAL] Process %d terminated\n", pid);
}

static void handle_pending_signals(tf_t *tf)
{
    unsigned int cur_pid = get_curid();
    uint32_t pending_signals = tcb_get_pending_signals(cur_pid);

    // Check for pending signals that aren't blocked
    if (pending_signals != 0) {
        KERN_INFO("[SIGNAL] Process %d has pending signals: 0x%x\n", cur_pid, pending_signals);
        for (int signum = 1; signum < NSIG; signum++) {
            if (pending_signals & (1 << signum)) {
                KERN_INFO("[SIGNAL] Processing signal %d for process %d\n", signum, cur_pid);
                // Clear the pending signal
                tcb_clear_pending_signal(cur_pid, signum);

                // SIGKILL cannot be caught - always terminates
                if (signum == SIGKILL) {
                    terminate_process(cur_pid);
                    // Switch to another process since this one is dead
                    // Use thread_exit() not thread_yield() - we don't want to re-queue the dead process
                    thread_exit();
                    // Should not return here, but just in case
                    return;
                }

                // Deliver the signal to handler
                deliver_signal(tf, signum);
                break;
            }
        }
    }
}

void trap (tf_t *tf)
{
    unsigned int cur_pid;
    unsigned int in_kernel;

    cur_pid = get_curid ();
    set_pdir_base (0); //switch to the kernel's page table.

    trap_cb_t f;

    f = TRAP_HANDLER[get_pcpu_idx()][tf->trapno];

    if (f){
            f(tf);
    } else {
            KERN_WARN("No handler for user trap 0x%x, process %d, eip 0x%08x. \n",
                            tf->trapno, cur_pid, tf->eip);
    }

    kstack_switch(cur_pid);

    // Handle any pending signals BEFORE switching to user page table
    // This is critical because handle_pending_signals needs to access
    // page tables via identity-mapped physical addresses
    handle_pending_signals(tf);

    set_pdir_base(cur_pid);

    trap_return(tf);
}
