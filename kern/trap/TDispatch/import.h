#ifndef _KERN_TRAP_TDISPATCH_H_
#define _KERN_TRAP_TDISPATCH_H_

#ifdef _KERN_

unsigned int syscall_get_arg1(tf_t *tf);
void sys_puts(tf_t *tf);
void sys_spawn(tf_t *tf);
void sys_yield(tf_t *tf);
void sys_produce(tf_t *tf);
void sys_consume(tf_t *tf);

/* Signal syscalls */
void sys_sigaction(tf_t *tf);
void sys_kill(tf_t *tf);
void sys_pause(tf_t *tf);
void sys_sigreturn(tf_t *tf);

/* Shell syscalls */
void sys_sync_send(tf_t *tf);
void sys_sync_recv(tf_t *tf);
void sys_is_dir(tf_t *tf);
void sys_ls(tf_t *tf);
void sys_pwd(tf_t *tf);
void sys_cp(tf_t *tf);
void sys_mv(tf_t *tf);
void sys_rm(tf_t *tf);
void sys_cat(tf_t *tf);
void sys_touch(tf_t *tf);
void sys_readline(tf_t *tf);

void syscall_set_errno(unsigned int errno);

#endif /* _KERN_ */

#endif /* !_KERN_TRAP_TDISPATCH_H_ */
