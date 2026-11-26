/*
 * lockhold.c - 鎖持有量測的 kernel backend(I-349 lock-hold,055;介面見 include/lockhold.h,契約 Codex 079)
 *
 * 狀態歸屬:
 *   lc[cpu].u.m / held / seen_gen   只由該顆 CPU 寫(hook 與 safepoint,都在關中斷下,本地排他)。
 *   lc[cpu].pub / pub_seq           只由該顆 CPU 寫(seqlock 寫者),任何 CPU 讀(lockhold_snapshot)。
 *   lc[cpu].ack                     只由該顆 CPU 寫(gen << 8 | 結果,一個 32 位元字,不會讀到半對)。
 *   rq                              只由控制端寫(ctl_busy CAS 保證同時只有一個),各 CPU 的 safepoint 讀。
 * seqlock 寫者:seq+1(奇)→ release 屏障 → 寫欄位 → release 屏障 → seq+1(偶);
 * 讀者:acquire 讀 seq → 讀欄位 → acquire 屏障 → 再讀 seq,相同且偶數才算一致(同 kernel/irq_late.c)。
 * i386 沒有 64 位元 __atomic,也不能用 libgcc:模型整塊以 32 位元字(union 型別雙關,C11 允許)
 * RELAXED 複製,一致性由 seqlock 保證;不用除法。
 * hook 零配置、零輸出、不取任何鎖;安全點的請求讀取只試一次(中斷內不迴圈)。
 */
#include <stdint.h>
#include "cpu.h"
#include "lockhold.h"
#include "timer.h"

/* 由 arch 提供(include/arch.h,另一個 agent 實作):讀一次合格的共用計數器,填 source / generation / valid */
void arch_lh_stamp(struct lh_stamp *out);

#define LD(p)     __atomic_load_n((p), __ATOMIC_RELAXED)
#define ST(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)

#define LH_WORDS ((sizeof(struct lh_model) + 3u) / 4u)
union lh_words {
    struct lh_model m;
    uint32_t w[LH_WORDS];
};
_Static_assert(sizeof(struct lh_model) == LH_WORDS * 4u && sizeof(union lh_words) == LH_WORDS * 4u,
               "lh_model 必須能整除成 32 位元字");

enum { CMD_NONE = 0, CMD_START = 1, CMD_STOP = 2 };
/* 迴圈次數上限:timer 不前進(例如目標就是推進 timer 的 CPU)時也保證返回 */
#define LH_SPIN_CAP (1u << 26)

struct lh_cpu {
    union lh_words u;        /* 本地模型(只本 CPU 寫) */
    uint32_t held;           /* 本 CPU 目前持有幾把目標鎖(hook 不論 phase 都 +1 / -1) */
    uint32_t seen_gen;       /* 已處理到的請求 gen */
    uint32_t panic_seen;     /* 已對本地模型標過 LH_PANIC */
    uint32_t pub_seq;        /* 發布副本的 seqlock */
    uint32_t pub_no;         /* 發布次數(seqlock 內) */
    union lh_words pub;      /* 發布副本 */
    uint32_t ack;            /* gen << 8 | enum lh_ack */
};

struct lh_req {
    uint32_t seq;            /* seqlock(只有控制端寫) */
    uint32_t gen_in;         /* seqlock 內的 gen 副本:讀到的欄位與 gen 成對 */
    uint32_t gen;            /* 給 CPU 快速判斷「有新請求」;release 發布 */
    uint32_t cmd, session, mask;
};

static struct lh_cpu lc[MAX_CPUS];
static struct lh_req rq;
static uint32_t ctl_busy;        /* 控制端互斥 */
static uint32_t last_session;    /* 最近一次發布 START 的 session(控制端寫;單調遞增) */
static uint32_t lh_panic_flag;

static inline struct lh_cpu *self(void)
{
    int id = this_cpu()->id;
    return (id >= 0 && id < MAX_CPUS) ? &lc[id] : 0;
}

/* PANIC 只由本 CPU 套在自己的模型上(遠端不改模型) */
static inline void check_panic(struct lh_cpu *c)
{
    if (!c->panic_seen && LD(&lh_panic_flag)) {
        c->panic_seen = 1;
        if (c->u.m.phase != LH_OFF)
            lh_fail(&c->u.m, LH_PANIC);
    }
}

void lockhold_acquired(uint8_t id)
{
    struct lh_cpu *c = self();
    if (!c)
        return;
    if (id >= 1 && id <= LH_LOCKS)
        c->held++;
    check_panic(c);
    if (c->u.m.phase != LH_ON)       /* 模型在非 ON 時本來就不記;省一次讀時鐘 */
        return;
    struct lh_stamp t;
    arch_lh_stamp(&t);
    lh_acquire(&c->u.m, id, t);
}

void lockhold_releasing(uint8_t id)
{
    struct lh_cpu *c = self();
    if (!c)
        return;
    if (id >= 1 && id <= LH_LOCKS) {
        if (c->held)
            c->held--;
        else
            lh_fail(&c->u.m, LH_INCONSISTENT);   /* 放開次數多於取得:配對已壞,留證據 */
    }
    check_panic(c);
    /* phase 不是 ON 也必須交給模型:DRAINING 要清舊 token、記 invalid。沒有 token 時才省讀時鐘 */
    struct lh_stamp t = {0};
    if (id >= 1 && id <= LH_LOCKS && c->u.m.token[id - 1].active)
        arch_lh_stamp(&t);
    lh_release(&c->u.m, id, t);
}

/* 本 CPU 把模型以 seqlock 發布成副本 */
static void publish(struct lh_cpu *c)
{
    uint32_t s = c->pub_seq;
    ST(&c->pub_seq, s + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (uint32_t i = 0; i < LH_WORDS; i++)
        ST(&c->pub.w[i], c->u.w[i]);
    ST(&c->pub_no, c->pub_no + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ST(&c->pub_seq, s + 2);
}

/* 讀一份一致的請求;只試一次 */
static int req_read(uint32_t *gen, uint32_t *cmd, uint32_t *session, uint32_t *mask)
{
    uint32_t s = __atomic_load_n(&rq.seq, __ATOMIC_ACQUIRE);
    if (s & 1)
        return 0;
    *gen = LD(&rq.gen_in);
    *cmd = LD(&rq.cmd);
    *session = LD(&rq.session);
    *mask = LD(&rq.mask);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return LD(&rq.seq) == s;
}

void lockhold_safepoint(void)
{
    struct lh_cpu *c = self();
    if (!c)
        return;
    check_panic(c);
    uint32_t pg = __atomic_load_n(&rq.gen, __ATOMIC_ACQUIRE);
    if (pg == c->seen_gen)
        return;
    uint32_t gen, cmd, session, mask;
    if (!req_read(&gen, &cmd, &session, &mask))
        return;                              /* 寫入中:留到下一次安全點 */
    c->seen_gen = gen;
    int cpu = (int)(c - lc);
    if (!(mask >> cpu & 1u))
        return;
    struct lh_model *m = &c->u.m;
    uint32_t res;
    if (c->held) {
        /* 安全點仍持有目標鎖:不得推定「進了中斷就沒持鎖」。不清 token、不 begin / stop、不 ack 成功 */
        lh_fail(m, LH_INCONSISTENT);
        res = LH_ACK_INCONSISTENT;
    } else if (cmd == CMD_START) {
        struct lh_stamp src;
        arch_lh_stamp(&src);
        res = lh_begin(m, session, src) ? LH_ACK_OK : LH_ACK_FAIL;
        /* lh_begin 清掉了模型(含 LH_PANIC);panic 已發生就立刻重標 */
        if (c->panic_seen)
            lh_fail(m, LH_PANIC);
    } else if (cmd == CMD_STOP) {
        struct lh_stamp src;
        arch_lh_stamp(&src);
        if (m->session != session) {
            lh_fail(m, LH_INCONSISTENT);
            res = LH_ACK_INCONSISTENT;
        } else {
            /* lh_stop 回 1 只表示本地 FROZEN;完整與否另看 flags 與全部 CPU 的 ACK */
            res = lh_stop(m, src) ? LH_ACK_OK : LH_ACK_FAIL;
        }
    } else {
        res = LH_ACK_FAIL;
    }
    publish(c);
    __atomic_store_n(&c->ack, gen << 8 | res, __ATOMIC_RELEASE);
}

/* 控制端:發布請求並等 ACK */
static int request(uint32_t cmd, uint32_t session, unsigned mask, uint32_t timeout_ticks,
                   uint8_t ack[MAX_CPUS])
{
    for (int i = 0; i < MAX_CPUS; i++)
        ack[i] = LH_ACK_NONE;
    uint32_t z = 0;
    if (!__atomic_compare_exchange_n(&ctl_busy, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        for (int i = 0; i < MAX_CPUS; i++)
            if (mask >> i & 1u)
                ack[i] = LH_ACK_BUSY;
        return 0;
    }
    if (cmd == CMD_START) {
        if (!session || session <= last_session) {   /* 不重用、不 wrap */
            for (int i = 0; i < MAX_CPUS; i++)
                if (mask >> i & 1u)
                    ack[i] = LH_ACK_FAIL;
            __atomic_store_n(&ctl_busy, 0u, __ATOMIC_RELEASE);
            return 0;
        }
        last_session = session;
    } else {
        session = last_session;
    }
    /* gen 只用低 24 位元放進 ack;遞增時跳過 0(0 = 初始值,CPU 會當成「沒有請求」) */
    uint32_t g = (LD(&rq.gen_in) + 1u) & 0xFFFFFFu;
    if (!g)
        g = 1;
    uint32_t s = rq.seq;
    ST(&rq.seq, s + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ST(&rq.gen_in, g);
    ST(&rq.cmd, cmd);
    ST(&rq.session, session);
    ST(&rq.mask, (uint32_t)mask);
    __atomic_store_n(&rq.gen, g, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&rq.seq, s + 2, __ATOMIC_RELEASE);

    uint64_t t0 = timer_ticks();
    unsigned pending = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!(mask >> i & 1u))
            continue;
        if (!cpus[i].online)
            ack[i] = LH_ACK_OFFLINE;         /* 離線必標不完整 */
        else
            pending |= 1u << i;
    }
    for (uint32_t spin = 0; pending; spin++) {
        for (int i = 0; i < MAX_CPUS; i++) {
            if (!(pending >> i & 1u))
                continue;
            uint32_t a = __atomic_load_n(&lc[i].ack, __ATOMIC_ACQUIRE);
            if (a >> 8 == g) {
                ack[i] = (uint8_t)(a & 0xFFu);
                pending &= ~(1u << i);
            }
        }
        if (!pending)
            break;
        if (timer_ticks() - t0 > timeout_ticks || spin >= LH_SPIN_CAP) {
            for (int i = 0; i < MAX_CPUS; i++)
                if (pending >> i & 1u)
                    ack[i] = LH_ACK_TIMEOUT;   /* 不強制釋鎖;之後的晚到套用不算完整 */
            break;
        }
        arch_cpu_relax();
    }
    __atomic_store_n(&ctl_busy, 0u, __ATOMIC_RELEASE);
    int all = 1;
    for (int i = 0; i < MAX_CPUS; i++)
        if ((mask >> i & 1u) && ack[i] != LH_ACK_OK)
            all = 0;
    return all;
}

int lockhold_start(uint32_t session, unsigned mask, uint32_t timeout_ticks, uint8_t ack[MAX_CPUS])
{
    return request(CMD_START, session, mask, timeout_ticks, ack);
}

int lockhold_stop(unsigned mask, uint32_t timeout_ticks, uint8_t ack[MAX_CPUS])
{
    return request(CMD_STOP, 0, mask, timeout_ticks, ack);
}

void lockhold_panic(void)
{
    __atomic_store_n(&lh_panic_flag, 1u, __ATOMIC_RELEASE);
}

void lockhold_snapshot(int cpu, struct lockhold_snap *out)
{
    out->ok = 0;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    struct lh_cpu *c = &lc[cpu];
    static union lh_words tmp[MAX_CPUS];  /* 不放 stack(約 2 KB);同一顆 cpu 的快照同時只有一個讀者使用 */
    union lh_words *o = &tmp[cpu];
    for (int tries = 0; tries < LH_SNAP_TRIES; tries++) {
        uint32_t s = __atomic_load_n(&c->pub_seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            arch_cpu_relax();
            continue;
        }
        for (uint32_t i = 0; i < LH_WORDS; i++)
            o->w[i] = LD(&c->pub.w[i]);
        out->pub_no = LD(&c->pub_no);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (LD(&c->pub_seq) == s) {
            out->m = o->m;
            out->panic = __atomic_load_n(&lh_panic_flag, __ATOMIC_ACQUIRE);
            if (out->panic)
                out->m.flags |= LH_PANIC;
            out->ok = 1;
            return;
        }
    }
}
