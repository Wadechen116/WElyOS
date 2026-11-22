/*
 * proc_signal.c - 訊號、暫停與繼續、訊號處理函式、返回使用者模式前的檢查
 *
 * 行程管理內部共用的定義(struct proc 等)在 proc_internal.h;對外的介面在 include/proc.h。
 */
#define PROC_OWNER_SIG              /* 這個檔案是這一組狀態的擁有者(見 proc_internal.h) */
#include "proc_internal.h"
#include "failinj.h"

/*
 * ---- 訊號 ----
 *
 * 訊號(signal)是「從外面通知一個行程」的方法。編號沿用 Unix 的定義。
 * 每個訊號有預設的效果;除了 SIGKILL 與 SIGSTOP,行程可以改成「忽略」或
 * 「執行自己的處理函式」(見下方「訊號處理函式」):
 *
 *   終止   SIGINT  (2)   使用者按了 Ctrl-C
 *          SIGKILL (9)   強制終止(不能攔截)
 *          SIGUSR1 (10)、SIGUSR2 (12)  沒有固定意義,留給程式自己使用
 *          SIGTERM (15)  一般的「請結束」(kill 指令的預設)
 *   暫停   SIGSTOP (19)  暫停(不能攔截)
 *          SIGTSTP (20)  使用者按了 Ctrl-Z
 *          SIGTTIN (21)  背景的行程想讀鍵盤(見 console.c)
 *   繼續   SIGCONT (18)  讓暫停中的行程繼續
 *
 * 被訊號終止的行程,結束碼是 128 + 訊號編號(Unix shell 的慣例),
 * 所以從結束碼看得出它是自己結束的還是被終止的。
 *
 * ---- 暫停與繼續 ----
 *
 * 暫停 = 行程還在(記憶體、開啟的檔案都保留),只是不再執行使用者模式的程式碼。
 * 做法和「終止」用的是同一個檢查點:每條執行緒要從 kernel 返回使用者模式之前
 * 都會經過 user_return_check,看到行程被暫停,就睡在 p->sig.stop_wq 上,等 SIGCONT。
 *
 *   - 正在使用者模式執行的執行緒:最晚在下一個計時器中斷(1ms)進入 kernel 時停下
 *   - 正睡在 kernel 裡的執行緒(等時間到、等管線…):不打斷它。它等的事情照常進行,
 *     醒來要回使用者模式時才停下。這樣不必讓每一種等待都處理「被暫停打斷」,
 *     系統呼叫也不會因為暫停而提早返回(程式看不出自己被暫停過)
 *   - 行程一被標記為暫停,父行程立刻得到通知(waitpid 的 WUNTRACED),
 *     不必等所有執行緒都真的停下:它們已經不可能再執行使用者模式的程式碼了
 *
 * 終止類的訊號對暫停中的行程立刻生效(不必先讓它繼續)。
 */
static int sig_valid(long sig)
{
    return sig == SIGINT || sig == SIGKILL || sig == SIGTERM || sig == SIGUSR1 || sig == SIGUSR2 ||
           sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN;
}

/* 行程可以自己決定怎麼處理的訊號 */
static int sig_catchable(long sig)
{
    return sig == SIGINT || sig == SIGTERM || sig == SIGUSR1 || sig == SIGUSR2 || sig == SIGTSTP;
}

/* 對一個行程送出訊號。呼叫時必須持有排程器的鎖;q 不可以是 zombie */
static void proc_signal_locked(struct proc *q, int sig)
{
    if (sig_catchable(sig) && q->sig.handler[sig] != SIG_DFL && !q->exiting) {
        if (q->sig.handler[sig] == SIG_IGN)
            return;                              /* 行程選擇忽略 */
        /* 行程有自己的處理函式:記下來,等它的某條執行緒要返回使用者模式時交給它
           (user_return_check)。正在 kernel 裡等待的執行緒先叫起來,不必等到它等的事情發生 */
        q->sig.pending |= 1u << sig;
        sched_interrupt_group(q);
        return;
    }
    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN) {
        if (q->exiting || q->sig.stopped)
            return;
        q->sig.stopped = sig;
        q->sig.stop_reported = 0;
        struct proc *parent = q->ppid ? proc_find(q->ppid) : NULL;
        if (parent)
            wq_wake_all(&parent->child_wq);  /* 父行程可能正在 waitpid(WUNTRACED) */
    } else if (sig == SIGCONT) {
        if (!q->sig.stopped)
            return;
        q->sig.stopped = 0;
        wq_wake_all(&q->sig.stop_wq);
        console_wake_readers();              /* 它可能是在等鍵盤輸入時被暫停的 */
    } else {
        proc_begin_exit(q, 128 + sig);
    }
}

int proc_signal_group_locked(int pgid, int sig)
{
    int n = 0;

    for (struct proc *q = procs; q; q = q->next) {
        if (q->pgid == pgid && !q->zombie) {
            proc_signal_locked(q, sig);
            n++;
        }
    }
    return n;
}

/* 目前這條執行緒所屬行程的群組;不屬於任何行程(kernel 執行緒)回傳 0。持有排程器的鎖時呼叫 */
int proc_self_pgid(void)
{
    struct proc *p = thread_data();

    return p ? p->pgid : 0;
}

/* 目前這條執行緒所屬的行程是不是被暫停了。持有排程器的鎖時呼叫 */
int proc_self_stopped(void)
{
    struct proc *p = thread_data();

    return p && p->sig.stopped;
}

/* 群組 pgid 裡還有沒有活著的行程。持有排程器的鎖時呼叫 */
int proc_group_alive_locked(int pgid)
{
    for (struct proc *q = procs; q; q = q->next)
        if (q->pgid == pgid && !q->zombie)
            return 1;
    return 0;
}

/* p 可不可以對 q 送訊號:同一個使用者的行程之間可以;對別人的行程需要 CAP_KILL */
static int may_signal(struct proc *p, struct proc *q)
{
    return (p->cred.caps & CAP_KILL) || p->cred.uid == q->cred.uid;
}

/* kill(pid, sig):pid > 0 是單一行程;pid < 0 是整個群組(編號為 -pid)。
   沒有權限回傳 -E_PERM(群組:裡面只要有一個可以送就算成功,不能送的略過) */
long sys_kill(struct proc *p, long pid, long sig)
{
    long rc = -E_SRCH;

    if (!sig_valid(sig) || pid == 0)
        return -E_INVAL;
    unsigned long flags = sched_lock();
    if (pid < 0) {
        for (struct proc *q = procs; q; q = q->next) {
            if (q->pgid != (int)-pid || q->zombie)
                continue;
            if (!may_signal(p, q)) {
                if (rc == -E_SRCH)
                    rc = -E_PERM;
                continue;
            }
            proc_signal_locked(q, (int)sig);
            rc = 0;
        }
    } else {
        struct proc *q = proc_find((int)pid);
        if (q && !q->zombie) {
            if (may_signal(p, q)) {
                proc_signal_locked(q, (int)sig);
                rc = 0;
            } else {
                rc = -E_PERM;
            }
        }
    }
    sched_preempt_point();
    sched_unlock(flags);
    return rc;
}

/* setpgid(pid, pgid):把自己(pid 為 0)或自己的子行程放進群組 pgid(0 表示以它自己的 pid 為群組) */
long sys_setpgid(struct proc *p, long pid, long pgid)
{
    long rc = -E_SRCH;
    unsigned long flags = sched_lock();
    struct proc *q = pid == 0 ? p : proc_find((int)pid);

    if (pgid < 0) {
        rc = -E_INVAL;
    } else if (q && (q == p || q->ppid == p->pid)) {
        q->pgid = pgid ? (int)pgid : q->pid;
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

int proc_kill(int pid, int code)
{
    unsigned long flags = sched_lock();
    struct proc *p = proc_find(pid);
    int rc = -1;

    if (p && !p->zombie) {
        proc_begin_exit(p, code);
        rc = 0;
    }
    sched_unlock(flags);
    return rc;
}

void user_fault(const char *what, uintptr_t pc, uintptr_t addr)
{
    struct proc *p = thread_data();

    kprintf("[pid %d %s] killed: %s at pc=%p addr=%p\n", p ? p->pid : 0,
            p ? p->name : "?", what, (void *)pc, (void *)addr);
    blackbox_log(BB_FAULT, "pid %d %s: %s at pc=%p addr=%p", p ? p->pid : 0, p ? p->name : "?", what,
                 (void *)pc, (void *)addr);
    proc_exit(PROC_KILLED_FAULT);
    for (;;)
        ;                            /* 不會到達;讓編譯器知道本函式不返回 */
}

/*
 * ---- 訊號處理函式 ----
 *
 * 行程可以登記「收到某個訊號時,執行我的這個函式」。困難在於:訊號隨時可能來,
 * 程式當時正在做別的事,暫存器裡都是有用的資料。處理函式跑完之後,
 * 程式必須從被打斷的地方、以完全相同的暫存器內容繼續。
 *
 * kernel 沒有另外的地方保存這些暫存器,做法是借用「使用者自己的 stack」:
 *
 *   1. 交付(deliver_signal):執行緒要返回使用者模式時,把它當時的暫存器(trap frame)
 *      整份複製到使用者 stack 上,然後把 trap frame 改成「呼叫 handler(sig)」。
 *      返回使用者模式後,CPU 執行的就是處理函式,用的是同一條 stack 的下面一段
 *   2. 處理函式 return:返回位址被設成一小段使用者端的程式(restorer),它只做一件事 —
 *      呼叫 sigreturn 系統呼叫,並告訴 kernel 那份暫存器放在哪裡
 *   3. sigreturn:把那份暫存器讀回來,取代目前的 trap frame。返回使用者模式後,
 *      程式就從當初被打斷的地方繼續,完全不知道中間發生過什麼
 *
 *        使用者 stack(往下長)
 *        | 被打斷時正在用的部分 |
 *        +----------------------+ <- 被打斷時的 sp
 *        | 存下來的暫存器        |    <- kernel 寫入;sigreturn 時讀回
 *        +----------------------+ <- frame
 *        | 處理函式自己用的部分  |
 *
 * 安全:那份暫存器放在使用者的記憶體裡,程式可以任意改它(也可以隨便捏造一份來呼叫
 * sigreturn)。所以讀回來的時候,「只有 kernel 能決定的部分」— 特權等級、中斷開關 —
 * 一律不採用(arch_frame_restore)。程式能改的只有它本來就能改的一般暫存器,
 * 最壞的結果是它自己跳到奇怪的地方而被終止。
 *
 * 訊號屬於整個行程:哪一條執行緒先走到返回使用者模式的檢查點,就由它執行處理函式。
 */
static void deliver_signal(struct proc *p, void *tf)
{
    _Alignas(16) char saved[512];
    uintptr_t words[2], handler, restorer;
    int sig = 0;

    unsigned long flags = sched_lock();
    thread_clear_interrupt();
    for (int s = 1; s < NSIG && !sig; s++) {
        if (p->sig.pending & (1u << s)) {
            p->sig.pending &= ~(1u << s);
            sig = s;
        }
    }
    handler = sig ? p->sig.handler[sig] : SIG_DFL;
    restorer = p->sig.restorer;
    sched_unlock(flags);
    if (!sig || handler == SIG_DFL || handler == SIG_IGN)
        return;                                  /* 被別條執行緒拿走了,或處理方式剛被改掉 */

    size_t size = arch_frame_size();
    /* 存放位置:目前的 stack pointer 之下,留一點空隙(有些呼叫慣例允許程式使用
       stack pointer 以下的一小段),對齊 16 */
    uintptr_t frame = (arch_frame_user_sp(tf) - 128 - size) & ~(uintptr_t)15;
    memcpy(saved, tf, size);
    unsigned n = arch_frame_call(tf, frame, handler, (uintptr_t)sig, restorer, words);
    uintptr_t new_sp = arch_frame_user_sp(tf);
    if (size > sizeof(saved) || copy_to_user(p, frame, saved, size) != 0 ||
        (n && copy_to_user(p, new_sp, words, n * sizeof(words[0])) != 0)) {
        /* 使用者的 stack 壞了(或已經用完):沒有地方放,只能照預設的方式處理 — 終止 */
        proc_exit(128 + sig);
    }
}

/* sigreturn:處理函式結束,還原被打斷時的暫存器。回傳值會被放進「系統呼叫回傳值」的
   暫存器,所以回傳還原之後那個暫存器原本的值,等於沒有動到它 */
long sys_sigreturn(struct proc *p, uintptr_t uframe, void *tf)
{
    _Alignas(16) char saved[512];
    size_t size = arch_frame_size();

    if (size > sizeof(saved) || copy_from_user(p, saved, uframe, size) != 0) {
        proc_exit(PROC_KILLED_FAULT);            /* 沒有可以還原的東西:無法繼續執行 */
        return 0;
    }
    arch_frame_restore(tf, saved);
    return arch_frame_get_retval(tf);
}

/* signal(sig, handler, restorer):設定處理方式,回傳原本的 */
long sys_signal(struct proc *p, long sig, uintptr_t handler, uintptr_t restorer)
{
    if (sig <= 0 || sig >= NSIG || !sig_catchable(sig))
        return -E_INVAL;                         /* SIGKILL 與 SIGSTOP 不能改:永遠有辦法停下一個程式 */
    /* 處理函式必須是使用者可以執行的位址;restorer 也是 */
    if (handler != SIG_DFL && handler != SIG_IGN &&
        (vm_space_query(p->space, handler, NULL, NULL) != 0 ||
         vm_space_query(p->space, restorer, NULL, NULL) != 0))
        return -E_FAULT;

    unsigned long flags = sched_lock();
    long old = (long)p->sig.handler[sig];
    p->sig.handler[sig] = handler;
    if (handler != SIG_DFL && handler != SIG_IGN)
        p->sig.restorer = restorer;
    else
        p->sig.pending &= ~(1u << sig);          /* 不再由函式處理:還沒交付的就不算了 */
    sched_unlock(flags);
    return old;
}

void user_return_check(void *tf)
{
    struct proc *p = thread_data();

    if (!p)
        return;
    /* 行程被暫停了:睡到有人讓它繼續(或終止它)為止。
       先不上鎖看一眼:這條路徑每次返回使用者模式都會經過,絕大多數時候沒有被暫停。
       剛好錯過也無妨 — 最晚下一個計時器中斷又會經過這裡 */
    if (p->sig.stopped && !thread_killed()) {
        unsigned long flags = sched_lock();
        while (p->sig.stopped && !thread_killed())
            wq_sleep(&p->sig.stop_wq);
        sched_unlock(flags);
    }
    if (thread_killed())
        proc_thread_exit();
    pmem_apply_injection(p);                     /* 故障注入(SYS_seu):等著翻轉的位元由行程自己在這裡動手 */
    int injected = 0;
    /* I-334:以前是「沒有待處理訊號才看打斷標記並清掉」,兩次都不上鎖讀。訊號若剛好在兩次讀取
       之間送達(送的一方在 sched_lk 內先設 pending 再設 interrupted),打斷標記被清掉、訊號卻沒交付,
       接下來的睡眠就睡滿。現在只要任一個成立就交給 deliver_signal:它在鎖內清打斷標記、
       同時重新讀 pending — 和送的一方在同一把鎖下,不會再有「清掉了卻沒看到」的空檔 */
    int pend = p->sig.pending != 0;              /* 不上鎖先看一眼:快速路徑,真正的決定在鎖內 */
    if (!pend)
        injected = failinj_sig_window(p);        /* 測試:撐大這個空檔,平常什麼都不做 */
    if (pend || thread_interrupted())
        deliver_signal(p, tf);
    /* 測試的那一次返回:核對不變量 — 回到使用者模式時,不可以有可交付的待處理訊號、
       打斷標記卻已經清掉(接下來的睡眠就不會被叫醒) */
    if (injected) {
        unsigned long flags = sched_lock();
        int bad = 0;
        for (int s = 1; s < NSIG; s++)
            if ((p->sig.pending & (1u << s)) && p->sig.handler[s] != SIG_DFL && p->sig.handler[s] != SIG_IGN)
                bad = 1;
        if (bad && !thread_interrupted())
            failinj_note_lost_wake();
        sched_unlock(flags);
    }
}

/* ------------------------------------------------------------------ */
/* 給行程核心(proc.c)使用的介面                                       */
/* 訊號的狀態(p->sig)只有這個檔案可以存取                              */
/* ------------------------------------------------------------------ */

/* fork:處理方式沿用父行程的;還沒交付的訊號不帶過去 */
void psig_fork(struct proc *child, const struct proc *parent)
{
    memcpy(child->sig.handler, parent->sig.handler, sizeof(child->sig.handler));
    child->sig.restorer = parent->sig.restorer;
}

/* exec:處理函式是舊程式裡的位址,全部還原成預設(「忽略」則保留);還沒交付的也丟掉 */
void psig_exec(struct proc *p)
{
    unsigned long flags = sched_lock();

    for (int s = 0; s < NSIG; s++)
        if (p->sig.handler[s] != SIG_IGN)
            p->sig.handler[s] = SIG_DFL;
    p->sig.pending = 0;
    sched_unlock(flags);
}

/* 被暫停了嗎?回傳造成暫停的訊號編號,沒有被暫停回傳 0。呼叫時持有排程器的鎖 */
int psig_stopped(const struct proc *p)
{
    return p->sig.stopped;
}

/* 被暫停了、而且這次暫停還沒有告訴過父行程(waitpid 的 WUNTRACED 只回報一次)。持有排程器的鎖 */
int psig_stop_unreported(const struct proc *p)
{
    return p->sig.stopped && !p->sig.stop_reported;
}

/* 記下「這次暫停已經回報了」,回傳造成暫停的訊號編號。持有排程器的鎖 */
int psig_report_stop(struct proc *p)
{
    p->sig.stop_reported = 1;
    return p->sig.stopped;
}

/* 測試用(I-334):行程有沒有可交付(處理方式不是預設、不是忽略)的待處理訊號。
   由 thread_sleep 在 sched_lk 內呼叫,sig 的欄位也由同一把鎖保護 */
static int sig_deliverable_locked(void *data)
{
    struct proc *p = data;
    for (int s = 1; s < NSIG; s++)
        if ((p->sig.pending & (1u << s)) && p->sig.handler[s] != SIG_DFL && p->sig.handler[s] != SIG_IGN)
            return 1;
    return 0;
}

void proc_sig_enable_sleep_check(void)
{
    sched_sleep_check = sig_deliverable_locked;
}
