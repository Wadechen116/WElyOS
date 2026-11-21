#ifndef WELYOS_PROC_CALLS_H
#define WELYOS_PROC_CALLS_H
/*
 * proc_calls.h - 行程管理各模組提供給彼此(以及系統呼叫層)呼叫的函式
 *
 * 這裡的 struct proc 是「不透明」的:只有宣告,沒有內容。
 * 只引入這個檔案的程式(kernel/syscall.c)因此沒有辦法讀寫行程的任何欄位,只能呼叫下面的函式 —
 * 模組之間的界線由編譯器把關,不是靠約定。
 * 需要看到 struct proc 內容的模組(kernel/proc*.c)引入的是 proc_internal.h,它會再引入這個檔案。
 *
 * 每個函式的說明寫在它的定義處;這裡依「由誰提供」分組,註明目前有誰在用。
 *
 * 上鎖與指標的規則(標在函式後面):
 *   〔持有排程器的鎖〕  呼叫時必須已經持有 sched_lock。行程的串列與每個行程的生命週期狀態由它保護
 *   〔借用的指標〕      回傳的 struct proc * 只在「仍然持有排程器的鎖」期間有效;放開之後那個行程可能
 *                      已經結束並被釋放,不可以留著用。要跨越放鎖的區間,記住 pid、之後重新尋找
 * 沒有標示的函式不要求呼叫者持有任何鎖(需要的話自己拿)。
 */
#include <stddef.h>
#include <stdint.h>

struct proc;
struct file;
struct args;
struct sysinfo;

/* ---- 行程核心(proc.c):身分、生命週期、執行緒、建立與等待、exec、fork、futex、行程清單 ---- */
struct proc *proc_find(int pid);   /* 〔持有排程器的鎖〕〔借用的指標〕 使用者:proc_signal.c、proc_mem.c */
void proc_thread_exit(void);   /* 使用者:proc_signal.c、syscall.c */
void proc_begin_exit(struct proc *p, int code);   /* 〔持有排程器的鎖〕 使用者:proc_signal.c */
void proc_exit(int code);   /* 使用者:proc_signal.c、syscall.c */
int proc_add_thread(struct proc *p, uintptr_t entry, uintptr_t arg, uintptr_t usp);   /* 使用者:syscall.c */
long proc_wait_child(struct proc *p, long pid, int *code, int nohang, int untraced);   /* 使用者:syscall.c */
long sys_spawn(struct proc *p, uintptr_t upath, uintptr_t uargv);   /* 使用者:syscall.c */
long sys_exec(struct proc *p, uintptr_t upath, uintptr_t uargv);   /* 使用者:syscall.c */
long sys_fork(struct proc *p, void *tf);   /* 使用者:syscall.c */
long sys_futex_wait(struct proc *p, uintptr_t uaddr, int val);   /* 使用者:syscall.c */
long sys_futex_wake(struct proc *p, uintptr_t uaddr);   /* 使用者:syscall.c */
int proc_pid(const struct proc *p);   /* 使用者:syscall.c */
int proc_pgid(const struct proc *p);   /* 使用者:syscall.c */
int proc_has_cap(const struct proc *p, uint32_t cap);   /* 使用者:syscall.c */
int proc_user_addr_ok(const struct proc *p, uintptr_t addr);   /* 使用者:syscall.c */
long proc_cred_call(struct proc *p, long num, long a0, long a1);   /* 使用者:syscall.c */
long sys_procinfo(struct proc *p, long after, uintptr_t uinfo);   /* 使用者:syscall.c */
long sys_sysinfo(struct proc *p, uintptr_t uinfo);   /* 使用者:syscall.c */

/* ---- 訊號(proc_signal.c) ---- */
long sys_kill(struct proc *p, long pid, long sig);   /* 使用者:syscall.c */
long sys_setpgid(struct proc *p, long pid, long pgid);   /* 使用者:syscall.c */
long sys_sigreturn(struct proc *p, uintptr_t uframe, void *tf);   /* 使用者:syscall.c */
long sys_signal(struct proc *p, long sig, uintptr_t handler, uintptr_t restorer);   /* 使用者:syscall.c */
void psig_fork(struct proc *child, const struct proc *parent);   /* 使用者:proc.c */
void psig_exec(struct proc *p);   /* 使用者:proc.c */
int psig_stopped(const struct proc *p);   /* 〔持有排程器的鎖〕 使用者:proc.c */
int psig_stop_unreported(const struct proc *p);   /* 〔持有排程器的鎖〕 使用者:proc.c */
int psig_report_stop(struct proc *p);   /* 〔持有排程器的鎖〕 使用者:proc.c */

/* ---- 記憶體(proc_mem.c):和使用者空間之間的複製、配額、對映、故障注入 ---- */
int copy_from_user(struct proc *p, void *dst, uintptr_t uaddr, size_t len);   /* 使用者:proc.c、proc_signal.c、syscall.c */
int copy_to_user(struct proc *p, uintptr_t uaddr, const void *src, size_t len);   /* 使用者:proc.c、proc_signal.c、proc_fs.c、syscall.c */
int copy_path_from_user(struct proc *p, char *dst, uintptr_t uaddr);   /* 使用者:proc.c、proc_fs.c */
long sys_memlimit(struct proc *p, long kb);   /* 使用者:syscall.c */
long sys_mmap(struct proc *p, uintptr_t len);   /* 使用者:syscall.c */
long sys_munmap(struct proc *p, uintptr_t addr);   /* 使用者:syscall.c */
long sys_sbrk(struct proc *p, long incr);   /* 使用者:syscall.c */
struct mb_domain *pmem_domain_new(const struct proc *parent);   /* 使用者:proc.c */
int pmem_init(struct proc *p, struct mb_domain *dom);   /* 使用者:proc.c */
struct mb_domain *pmem_domain(const struct proc *p);   /* 使用者:proc.c */
void pmem_release(struct proc *p);   /* 使用者:proc.c */
void pmem_exec(struct proc *p);   /* 使用者:proc.c */
struct vm_space *pmem_fork(struct proc *child, struct proc *parent);   /* 使用者:proc.c */
long sys_seu(struct proc *p, long pid, uintptr_t addr, long bit);   /* 使用者:syscall.c */
void pmem_apply_injection(struct proc *p);   /* 使用者:proc_signal.c */

/* ---- 檔案環境(proc_fs.c):檔案描述符表、目前的工作目錄 ---- */
int fd_install(struct proc *p, struct file *f);   /* 使用者:syscall.c */
struct file *fd_get(struct proc *p, long fd);   /* 使用者:syscall.c */
struct file *fd_remove(struct proc *p, long fd);   /* 使用者:syscall.c */
void fd_close_all(struct proc *p);   /* 使用者:proc.c */
int fd_setup_console(struct proc *p);   /* 使用者:proc.c */
void fd_inherit(struct proc *p, struct proc *parent, unsigned count);   /* 使用者:proc.c */
int fd_in_range(long fd);   /* 使用者:syscall.c */
struct file *fd_replace(struct proc *p, long fd, struct file *f);   /* 使用者:syscall.c */
void files_init_cwd(struct proc *p, struct proc *parent);   /* 使用者:proc.c */
int user_path(struct proc *p, char *dst, uintptr_t uaddr);   /* 使用者:proc.c、syscall.c */
long sys_chdir(struct proc *p, uintptr_t upath);   /* 使用者:syscall.c */
long sys_getcwd(struct proc *p, uintptr_t ubuf, size_t len);   /* 使用者:syscall.c */
#endif
