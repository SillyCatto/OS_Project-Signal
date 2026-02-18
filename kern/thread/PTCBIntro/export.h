#ifndef _KERN_THREAD_PTCBINTRO_H_
#define _KERN_THREAD_PTCBINTRO_H_

#ifdef _KERN_

unsigned int tcb_get_state(unsigned int pid);
void tcb_set_state(unsigned int pid, unsigned int state);
unsigned int tcb_get_prev(unsigned int pid);
void tcb_set_prev(unsigned int pid, unsigned int prev_pid);
unsigned int tcb_get_next(unsigned int pid);
void tcb_set_next(unsigned int pid, unsigned int next_pid);
void tcb_init_at_id(unsigned int cpu_idx, unsigned int pid);

void* tcb_get_chan(unsigned int pid);
void tcb_set_chan(unsigned int pid, void *state);

#include <kern/fs/stat.h>
#include <kern/fs/dinode.h>
#include <kern/fs/inode.h>
#include <kern/fs/file.h>
#include <lib/signal.h>

struct file** tcb_get_openfiles(unsigned int pid);
void tcb_set_openfiles(unsigned int pid, int fd, struct file* f);
struct inode* tcb_get_cwd(unsigned int pid);
void tcb_set_cwd(unsigned int pid, struct inode* d);

/* Signal accessor functions */
struct sigaction* tcb_get_sigaction(unsigned int pid, int signum);
void tcb_set_sigaction(unsigned int pid, int signum, struct sigaction *act);
uint32_t tcb_get_pending_signals(unsigned int pid);
void tcb_set_pending_signals(unsigned int pid, uint32_t signals);
void tcb_add_pending_signal(unsigned int pid, int signum);
void tcb_clear_pending_signal(unsigned int pid, int signum);
void tcb_set_signal_context(unsigned int pid, uint32_t saved_esp_addr, uint32_t saved_eip_addr);
void tcb_get_signal_context(unsigned int pid, uint32_t *saved_esp_addr, uint32_t *saved_eip_addr);
void tcb_clear_signal_context(unsigned int pid);
int tcb_in_signal_handler(unsigned int pid);
uint32_t tcb_is_sleeping(unsigned int pid);
void* tcb_get_channel(unsigned int pid);

#endif /* _KERN_ */

#endif /* !_KERN_THREAD_PTCBINTRO_H_ */

