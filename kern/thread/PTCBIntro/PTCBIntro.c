#include <lib/x86.h>
#include <lib/debug.h>
#include <lib/thread.h>
#include <lib/string.h>
#include <lib/signal.h>
#include "export.h"

#include <kern/fs/params.h>
#include <kern/fs/stat.h>
#include <kern/fs/dinode.h>
#include <kern/fs/inode.h>
#include <kern/fs/path.h>
#include <kern/fs/file.h>


/**
 * The structure for the thread control block (TCB).
 * We are storing the set of TCBs in doubly linked lists.
 * To this purpose, in addition to the thread state, you also
 * need to save the thread (process) id of the previous and
 * the next TCB.
 * Since the value 0 is reserved for thread id 0, we use NUM_IDS
 * to represent the NULL index.
 */
struct TCB {
  t_state state;
  unsigned int prev;
  unsigned int next;
  void *channel;
  struct file *openfiles[NOFILE];  // Open files
  struct inode *cwd;               // Current working directory
  struct sig_state sigstate;       // Signal state for this process
};

struct TCB TCBPool[NUM_IDS];


unsigned int tcb_get_state(unsigned int pid)
{
	return TCBPool[pid].state;
}

void tcb_set_state(unsigned int pid, unsigned int state)
{
  //KERN_INFO("_____0_____ tcb_set_state: %d -> %d\n", pid, state);
  TCBPool[pid].state = state;
}

unsigned int tcb_get_prev(unsigned int pid)
{
	return TCBPool[pid].prev;
}

void tcb_set_prev(unsigned int pid, unsigned int prev_pid)
{
	TCBPool[pid].prev = prev_pid;
}

unsigned int tcb_get_next(unsigned int pid)
{
	return TCBPool[pid].next;
}

void tcb_set_next(unsigned int pid, unsigned int next_pid)
{
	TCBPool[pid].next = next_pid;
}

void tcb_init_at_id(unsigned int cpu_idx, unsigned int pid)
{
	TCBPool[pid].state = TSTATE_DEAD;
	TCBPool[pid].prev = NUM_IDS;
	TCBPool[pid].next = NUM_IDS;
	TCBPool[pid].channel = NULL;
	memset(TCBPool[pid].openfiles, 0, sizeof(TCBPool[pid].openfiles));
	TCBPool[pid].cwd = NULL;
	// Initialize signal state
	memset(&TCBPool[pid].sigstate, 0, sizeof(struct sig_state));
}

/*** NEW ***/

void* tcb_get_chan(unsigned int pid)
{
  return TCBPool[pid].channel;
}

void tcb_set_chan(unsigned int pid, void *chan)
{
  TCBPool[pid].channel = chan;
}

struct file** tcb_get_openfiles(unsigned int pid)
{
  return TCBPool[pid].openfiles;
}

void tcb_set_openfiles(unsigned int pid, int fd, struct file* f)
{
  (TCBPool[pid].openfiles)[fd] = f;
}

struct inode* tcb_get_cwd(unsigned int pid)
{
  return TCBPool[pid].cwd;
}

void tcb_set_cwd(unsigned int pid, struct inode* d)
{
  TCBPool[pid].cwd = d;
}

/*** Signal Accessors ***/

struct sigaction* tcb_get_sigaction(unsigned int pid, int signum)
{
  if (signum < 0 || signum >= NSIG)
    return NULL;
  return &TCBPool[pid].sigstate.sigactions[signum];
}

void tcb_set_sigaction(unsigned int pid, int signum, struct sigaction *act)
{
  if (signum >= 0 && signum < NSIG && act != NULL) {
    TCBPool[pid].sigstate.sigactions[signum] = *act;
  }
}

uint32_t tcb_get_pending_signals(unsigned int pid)
{
  return TCBPool[pid].sigstate.pending_signals;
}

void tcb_set_pending_signals(unsigned int pid, uint32_t signals)
{
  TCBPool[pid].sigstate.pending_signals = signals;
}

void tcb_add_pending_signal(unsigned int pid, int signum)
{
  if (signum >= 0 && signum < NSIG) {
    TCBPool[pid].sigstate.pending_signals |= (1 << signum);
  }
}

void tcb_clear_pending_signal(unsigned int pid, int signum)
{
  if (signum >= 0 && signum < NSIG) {
    TCBPool[pid].sigstate.pending_signals &= ~(1 << signum);
  }
}

void tcb_set_signal_context(unsigned int pid, uint32_t saved_esp_addr, uint32_t saved_eip_addr)
{
  TCBPool[pid].sigstate.saved_esp_addr = saved_esp_addr;
  TCBPool[pid].sigstate.saved_eip_addr = saved_eip_addr;
  TCBPool[pid].sigstate.in_signal_handler = 1;
}

void tcb_get_signal_context(unsigned int pid, uint32_t *saved_esp_addr, uint32_t *saved_eip_addr)
{
  *saved_esp_addr = TCBPool[pid].sigstate.saved_esp_addr;
  *saved_eip_addr = TCBPool[pid].sigstate.saved_eip_addr;
}

void tcb_clear_signal_context(unsigned int pid)
{
  TCBPool[pid].sigstate.saved_esp_addr = 0;
  TCBPool[pid].sigstate.saved_eip_addr = 0;
  TCBPool[pid].sigstate.in_signal_handler = 0;
}

int tcb_in_signal_handler(unsigned int pid)
{
  return TCBPool[pid].sigstate.in_signal_handler;
}

uint32_t tcb_is_sleeping(unsigned int pid)
{
  return (TCBPool[pid].state == TSTATE_SLEEP) ? 1 : 0;
}

void* tcb_get_channel(unsigned int pid)
{
  return TCBPool[pid].channel;
}
