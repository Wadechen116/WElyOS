/*
 * clksync.c - 開機時驗證各 CPU 的共用計數器是否一致(I-337;規則見 include/clksync.h 與
 * tests/host/proc/probe_i337_counter_sync.c)
 *
 * 交接方式(無鎖,只用共用的 volatile 變數與一個 IPI):
 *   參考 CPU:設定 sx_target、sx_req = 新序號,發 IPI_CLKSYNC,等對方把 sx_ready 設成同一個序號。
 *   每次取樣:參考 CPU 讀 ref_before → 寫 sx_req = 新序號 → 等 sx_ack == 序號 → 讀 ref_after。
 *             對方 CPU 看到 sx_req 變了 → 讀 remote → 寫 sx_remote → 寫 sx_ack = 序號。
 *   結束:參考 CPU 設 sx_stop,等對方離開(sx_inside == 0)。
 * 兩邊的每一次等待都有以計數器量的時限,任何一方停住,另一方都不會無限等。
 *
 * 為什麼不用 smp_call_mask:它要等對方「做完」才返回,參考 CPU 在等待期間沒辦法同時讀自己的計數器;
 * 而且它的回呼「必須很短」,這裡需要一來一往好幾次。改用專屬的 IPI 位元與自己的時限。
 *
 * 為什麼沒有 64 位元除法:i386 沒有 __udivdi3。容許值 tol = 每 tick 計數 >> 2(四分之一 tick),
 * 時限 = 每 tick 計數 × 20,都只用移位與乘法;換算成千分之一 tick 時先把除數移位到 16 位元以內,
 * 再做 32 位元除法(見 clksync_milli_ticks)。
 */
#include <stddef.h>
#include "arch.h"
#include "blackbox.h"
#include "clksync.h"
#include "cpu.h"
#include "kprintf.h"
#include "sched.h"
#include "smp.h"
#include "timer.h"

#define CLKSYNC_WAIT_TICKS 20u   /* 每一次等待對方的時限(tick 數,換成計數用乘法) */

/* ---- 交接用的共用變數(參考 CPU 寫、對方 CPU 讀,或反過來) ---- */
static volatile int      sx_target = -1;     /* 這一輪由哪一顆 CPU 回應;-1 = 沒有 */
static volatile uint32_t sx_req, sx_ack, sx_ready;
static volatile int      sx_stop, sx_inside;
static volatile uint64_t sx_remote;          /* 對方的計數;寫完才寫 sx_ack(SEQ_CST),讀的一方先看到 ack 才讀 */
static volatile uint64_t sx_timeout;         /* 每次等待的時限(計數);發 IPI 之前寫好 */
static uint32_t sx_seq;                      /* 序號:只有參考 CPU 改(sx_busy 保證同一時間只有一個) */
static int sx_busy;                          /* 1 = 有一次驗證正在進行 */

/* ---- 測試注入(只有 selftest 會設定;平常 sx_inj_cpu == -1) ---- */
static volatile int      sx_inj_cpu = -1;
static volatile uint64_t sx_inj_off, sx_inj_delay;

/* ---- 結果 ---- */
static volatile int g_state = CLKSYNC_UNCHECKED;
static struct clksync_result g_res[3];      /* 0 開機、1 再驗證、2 換到備援之後 */
static volatile int g_have[3];
static volatile int g_fallback;              /* 1 = 已換到備援計數器而且合格 */
static int g_force;                          /* enum clksync_force_mode;只在 smp_init 之前設定 */
static uint64_t g_ok_at;                     /* 最近一次合格驗證開始時的計數(目前來源的單位) */

/*
 * 讀計數器:讀取指令本身也要依程式順序,不可被提前或延後到交接旗標的存取之外(I-342)。
 * 以前只用記憶體屏障:它排序記憶體存取,管不到 RDTSC / CNTPCT_EL0 / time CSR 這類讀取指令 —
 * 開機第一次檢查時 CPU 1 的讀數曾晚於「之後」的參考讀數(偏差 0.07 ~ 0.18 tick),複檢時才接近 0。
 * arch_clock_count_ordered 以各架構的序列化指令夾住讀取(見 arch.h)。
 */
static uint64_t read_count(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t c = arch_clock_count_ordered();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return c;
}

/* 下一個序號(跳過 0:sx_ready 清成 0 代表「還沒回應」) */
static uint32_t next_seq(void)
{
    if (++sx_seq == 0)
        sx_seq = 1;
    return sx_seq;
}

/*
 * 對方 CPU:收到 IPI_CLKSYNC 時在中斷處理中執行(中斷關閉)。不取任何鎖。
 * 每一次等待最多 sx_timeout 個計數;參考 CPU 停住時自己離開。
 */
void clksync_remote(void)
{
    int me = this_cpu()->id;

    if (__atomic_load_n(&sx_target, __ATOMIC_SEQ_CST) != me)
        return;                                  /* 過時的 IPI(那一輪已經結束):不理 */
    __atomic_store_n(&sx_inside, 1, __ATOMIC_SEQ_CST);
    uint64_t to = sx_timeout;
    uint32_t last = __atomic_load_n(&sx_req, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sx_ready, last, __ATOMIC_SEQ_CST);

    for (;;) {
        uint64_t start = read_count();
        uint32_t r;
        while ((r = __atomic_load_n(&sx_req, __ATOMIC_SEQ_CST)) == last) {
            if (__atomic_load_n(&sx_stop, __ATOMIC_SEQ_CST) || read_count() - start > to)
                goto out;
            arch_cpu_relax();
        }
        last = r;
        uint64_t c = read_count();
        if (__atomic_load_n(&sx_inj_cpu, __ATOMIC_SEQ_CST) == me) {
            c += sx_inj_off;                     /* 測試:假裝這顆 CPU 的計數器超前 */
            uint64_t d = sx_inj_delay, t = read_count();
            while (d && read_count() - t < d)
                arch_cpu_relax();                /* 測試:讀完後拖延才回應,把括號撐寬 */
        }
        sx_remote = c;
        __atomic_store_n(&sx_ack, r, __ATOMIC_SEQ_CST);
    }
out:
    __atomic_store_n(&sx_inside, 0, __ATOMIC_SEQ_CST);
}

/* 參考 CPU:等 *v == want,期間處理別顆 CPU 對我們的要求(避免互等)。逾時回傳 0 */
static int wait_eq(volatile uint32_t *v, uint32_t want, uint64_t to)
{
    uint64_t start = read_count();
    while (__atomic_load_n(v, __ATOMIC_SEQ_CST) != want) {
        if (read_count() - start > to)
            return 0;
        smp_poll();
        arch_cpu_relax();
    }
    return 1;
}

static int wait_out(uint64_t to)
{
    uint64_t start = read_count();
    while (__atomic_load_n(&sx_inside, __ATOMIC_SEQ_CST)) {
        if (read_count() - start > to)
            return 0;
        smp_poll();
        arch_cpu_relax();
    }
    return 1;
}

struct bracket { uint64_t before, remote, after; };

/* 對第 cpu 顆取 CLKSYNC_SAMPLES 次括號取樣(中斷關閉中、由參考 CPU 呼叫) */
static int sample_cpu(int cpu, struct bracket *s, uint64_t to)
{
    if (!wait_out(to))
        return CLKSYNC_R_NO_RESPONSE;            /* 上一輪的對方還沒離開 */
    sx_timeout = to;
    __atomic_store_n(&sx_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sx_ready, 0, __ATOMIC_SEQ_CST);
    uint32_t q = next_seq();
    __atomic_store_n(&sx_req, q, __ATOMIC_SEQ_CST);
    __atomic_store_n(&sx_target, cpu, __ATOMIC_SEQ_CST);
    smp_send(&cpus[cpu], IPI_CLKSYNC);

    int r = CLKSYNC_R_NONE;
    if (!wait_eq(&sx_ready, q, to)) {
        r = CLKSYNC_R_NO_RESPONSE;
        goto done;
    }
    for (unsigned k = 0; k < CLKSYNC_SAMPLES; k++) {
        q = next_seq();
        uint64_t b = read_count();
        __atomic_store_n(&sx_req, q, __ATOMIC_SEQ_CST);
        if (!wait_eq(&sx_ack, q, to)) {
            r = CLKSYNC_R_NO_RESPONSE;
            goto done;
        }
        uint64_t a = read_count();
        s[k].before = b;
        s[k].remote = sx_remote;
        s[k].after = a;
    }
done:
    __atomic_store_n(&sx_stop, 1, __ATOMIC_SEQ_CST);
    wait_out(to);                                /* 等對方離開;逾時時下一輪開頭會再檢查 */
    __atomic_store_n(&sx_target, -1, __ATOMIC_SEQ_CST);
    return r;
}

/*
 * 判定一顆 CPU 的一輪取樣(移植自 probe_i337_counter_sync.c 的 counter_pair_acceptable,
 * 加上「太寬的取樣不採用」):
 *   - 三個值各自嚴格遞增,否則 NONMONOTONIC
 *   - 寬度 > tol 的取樣不採用(偏移區間太寬,中點落在範圍內也不能算數)
 *   - 採用的少於一半:WIDE(呼叫端重試)
 *   - 採用的取樣中 min(remote - after) >= -tol 且 max(remote - before) <= tol,否則 OFFSET
 *     (整輪一起看:固定偏移與這段期間的漂移都會被抓到)
 * 差值以無號相減再轉成有號:64 位元計數器不會繞回,差值遠小於 2^63。
 */
static int judge(const struct bracket *s, uint64_t tol, struct clksync_cpu *o)
{
    o->min_low = INT64_MAX;
    o->max_high = INT64_MIN;
    o->tightest = UINT64_MAX;
    o->used = o->wide = 0;
    for (unsigned i = 0; i < CLKSYNC_SAMPLES; i++) {
        if (s[i].after < s[i].before)
            return CLKSYNC_R_NONMONOTONIC;
        if (i && (s[i].before <= s[i - 1].before || s[i].after <= s[i - 1].after ||
                  s[i].remote <= s[i - 1].remote))
            return CLKSYNC_R_NONMONOTONIC;
        uint64_t w = s[i].after - s[i].before;
        if (w < o->tightest)
            o->tightest = w;
        if (w > tol) {
            o->wide++;
            continue;
        }
        /* 這個樣本推出:遠端計數器的偏差 δ(遠端 − 參考,同一瞬間)落在 [remote − after, remote − before]。
           真正的 δ 必須同時落在每個樣本的範圍裡 → 取交集:下限取最大、上限取最小(I-342)。
           以前取聯集(下限取最小、上限取最大),等於要求每個樣本的整個區間都在容許值內,
           把跨 CPU 交接的延遲(區間寬度)誤算成偏差:CPU 1 第一次交接走冷路徑時被報成 0.07 ~ 0.18 tick */
        int64_t low = (int64_t)(s[i].remote - s[i].after);
        int64_t high = (int64_t)(s[i].remote - s[i].before);
        if (o->used == 0 || low > o->min_low)
            o->min_low = low;                    /* 交集的下限(名稱沿用:現在是「最大的下限」) */
        if (o->used == 0 || high < o->max_high)
            o->max_high = high;                  /* 交集的上限(現在是「最小的上限」) */
        o->used++;
    }
    if (o->used * 2 < CLKSYNC_SAMPLES)
        return CLKSYNC_R_WIDE;
    if (o->min_low > o->max_high)
        return CLKSYNC_R_NONMONOTONIC;          /* 各樣本互相矛盾:沒有一個 δ 能同時滿足(漂移或讀數錯亂) */
    /* 最壞情況的偏差:交集兩端絕對值的較大者(Codex 205:以前回報的是離 0 最近的一端,卻標成 max) */
    uint64_t lo = o->min_low < 0 ? (uint64_t)-o->min_low : (uint64_t)o->min_low;
    uint64_t hi = o->max_high < 0 ? (uint64_t)-o->max_high : (uint64_t)o->max_high;
    o->max_abs = lo > hi ? lo : hi;
    /* 不確定就判失敗(Codex 205):整個可能偏差的交集都要在 ±tol 之內。只要求「和 ±tol 有重疊」的話,
       交集 [tol − 10, tol + 10] 也會被接受,而真實偏差可能已經超過 tol。交集的寬度不超過最窄的那個
       樣本區間(實測約 2000 計數,遠小於 tol),所以這不會再把交接延遲算進去 */
    if (o->min_low < -(int64_t)tol || o->max_high > (int64_t)tol)
        return CLKSYNC_R_OFFSET;
    return CLKSYNC_R_NONE;
}

/* 一次完整驗證:參考 CPU = 呼叫者所在的 CPU,其他每一顆已啟動的 CPU 都要合格 */
static int run(struct clksync_result *res)
{
    struct bracket s[CLKSYNC_SAMPLES];

    for (unsigned i = 0; i < sizeof(*res); i++)
        ((char *)res)[i] = 0;
    res->bad_cpu = -1;
    res->ncpu = smp_started();
    res->iv = timer_tick_counts();
    if (res->iv == 0 || timer_clock_hz() == 0) {
        res->reason = CLKSYNC_R_NO_COUNTER;
        return 0;
    }
    if (__atomic_exchange_n(&sx_busy, 1, __ATOMIC_SEQ_CST)) {
        res->reason = CLKSYNC_R_BUSY;
        return 0;
    }
    res->tol = res->iv >> 2;
    uint64_t to = res->iv * CLKSYNC_WAIT_TICKS;

    /* 整次驗證都關中斷:參考 CPU 不能換人。最壞情形(對方全部不回應)約
       (CPU 數 - 1) × 重試次數 × 2 × 20 tick,遠小於看門狗的 5000 tick */
    unsigned long fl = arch_irq_save();
    res->ref = this_cpu()->id;
    res->src = timer_clock_name();
    res->at = read_count();
    res->ok = 1;
    for (int c = 0; c < res->ncpu; c++) {
        if (c == res->ref || (c != 0 && !cpus[c].started))   /* 第 0 顆是開機的 CPU,沒有 started 旗標 */
            continue;
        struct clksync_cpu *o = &res->cpu[c];
        for (o->attempts = 1; ; o->attempts++) {
            o->reason = sample_cpu(c, s, to);
            if (o->reason == CLKSYNC_R_NONE)
                o->reason = judge(s, res->tol, o);
            if ((o->reason != CLKSYNC_R_WIDE && o->reason != CLKSYNC_R_NO_RESPONSE) ||
                o->attempts >= CLKSYNC_ATTEMPTS)
                break;                           /* 只有「無法判斷」才重試;偏移與不單調是確定的失敗 */
        }
        if (o->reason != CLKSYNC_R_NONE && res->ok) {
            res->ok = 0;
            res->reason = o->reason;
            res->bad_cpu = c;
        }
    }
    arch_irq_restore(fl);
    __atomic_store_n(&sx_busy, 0, __ATOMIC_SEQ_CST);
    return res->ok;
}

uint32_t clksync_milli_ticks(uint64_t counts, uint64_t iv)
{
    if (iv == 0)
        return 0;
    if (counts >= iv * 4)
        return 4000;                             /* 上限:超過 4 tick 就只說「很多」 */
    uint64_t num = counts * 1000;                /* < 4000 × iv */
    while (iv > 0xFFFF) {                        /* 除數縮到 16 位元以內,被除數一起縮:比值不變(捨去誤差 < 0.1%) */
        iv >>= 1;
        num >>= 1;
    }
    return (uint32_t)num / (uint32_t)iv;         /* num < 4000 × 65536 < 2^32:32 位元除法 */
}

const char *clksync_reason_name(int r)
{
    switch (r) {
    case CLKSYNC_R_NONE:         return "ok";
    case CLKSYNC_R_NO_COUNTER:   return "no shared counter";
    case CLKSYNC_R_NO_RESPONSE:  return "cpu did not answer the handshake";
    case CLKSYNC_R_NONMONOTONIC: return "counter samples not strictly increasing";
    case CLKSYNC_R_WIDE:         return "every bracket too wide to judge";
    case CLKSYNC_R_OFFSET:       return "counter offset beyond a quarter tick";
    case CLKSYNC_R_BUSY:         return "another check in progress";
    case CLKSYNC_R_FORCED:       return "forced by boot parameter (test)";
    }
    return "?";
}

static const char *const check_name[3] = { "#1", "#2", "fallback" };
#define FALLBACK_HOPS_MAX 4      /* 備援鏈最多幾層(i386 兩層;多留餘裕,迴圈一定結束) */

/* 測試:clock=pmtimer 時,名稱不是 pmtimer 開頭的來源都判不合格(真正的驗證照做、照印) */
static int forced_reject(int ok, struct clksync_result *r)
{
    const char *n = timer_clock_name();
    int is_pm = n[0] == 'p' && n[1] == 'm';
    if (g_force == CLKSYNC_FORCE_PMTIMER && !is_pm && r->reason != CLKSYNC_R_NO_COUNTER) {
        r->ok = 0;
        r->reason = CLKSYNC_R_FORCED;
        r->bad_cpu = -1;
        return 0;
    }
    return ok;
}

static void print_result(int which, const struct clksync_result *r)
{
    for (int c = 0; c < r->ncpu; c++) {
        const struct clksync_cpu *o = &r->cpu[c];
        if (c == r->ref || (c != 0 && !cpus[c].started))
            continue;
        kprintf("clksync: check %s (%s) cpu %d vs cpu %d: max offset %u/1000 tick (%llu counts, "
                "range %lld..%lld), tightest bracket %llu counts, used %u/%u, wide %u, attempts %u: %s\n",
                check_name[which], r->src ? r->src : "?", c, r->ref, clksync_milli_ticks(o->max_abs, r->iv),
                (unsigned long long)o->max_abs, (long long)o->min_low, (long long)o->max_high,
                (unsigned long long)o->tightest, o->used, CLKSYNC_SAMPLES, o->wide, o->attempts,
                clksync_reason_name(o->reason));
    }
}

/* 失敗即關閉:停用共用路徑、記黑盒子、印出看得到的開機訊息 */
static void degrade(const char *what, int reason, int bad_cpu)
{
    timer_shared_disable();
    __atomic_store_n(&g_state, CLKSYNC_DEGRADED, __ATOMIC_SEQ_CST);
    kprintf("clksync: DEGRADED (check %s, counter %s): %s (cpu %d); system time is advanced by cpu 0 only\n",
            what, timer_clock_name(), clksync_reason_name(reason), bad_cpu);
    blackbox_log(BB_NOTE, "clksync degraded (check %s, %s): %s, cpu %d; cpu0-only time",
                 what, timer_clock_name(), clksync_reason_name(reason), bad_cpu);
}

/*
 * 一次驗證不合格(I-341):依序換到下一層備援計數器並完整驗證(i386:TSC → HPET → PM timer);
 * 每一層都不行、或沒有下一層時才降級。g_res[2] 保存最後一個嘗試的來源的結果。
 */
static void failed(int which, const struct clksync_result *r)
{
    const char *first = timer_clock_name();
    kprintf("clksync: check %s rejected counter %s: %s (cpu %d)\n", check_name[which], first,
            clksync_reason_name(r->reason), r->bad_cpu);
    const char *check = check_name[which];
    int reason = r->reason, bad = r->bad_cpu, ok = 0;
    struct clksync_result *f = &g_res[2];
    for (int hop = 0; hop < FALLBACK_HOPS_MAX && !ok; hop++) {
        const char *old = timer_clock_name();
        if (g_force == CLKSYNC_FORCE_CPU0ONLY || !timer_clock_fallback())
            break;
        kprintf("clksync: switched shared counter %s -> %s (%llu Hz); validating it\n", old, timer_clock_name(),
                (unsigned long long)timer_clock_hz());
        ok = forced_reject(run(f), f);
        __atomic_store_n(&g_have[2], 1, __ATOMIC_SEQ_CST);
        print_result(2, f);
        check = check_name[2];
        reason = f->reason;
        bad = f->bad_cpu;
    }
    if (!ok) {
        degrade(check, reason, bad);
        return;
    }
    g_ok_at = f->at;
    __atomic_store_n(&g_fallback, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_state, CLKSYNC_OK, __ATOMIC_SEQ_CST);
    kprintf("clksync: fallback counter %s ok on %d cpus (tolerance %llu counts = 1/4 tick)\n",
            f->src, f->ncpu, (unsigned long long)f->tol);
    blackbox_log(BB_NOTE, "clksync: %s rejected (%s, cpu %d); shared counter now %s, validated",
                 first, clksync_reason_name(r->reason), r->bad_cpu, f->src);
}

static void live(int which)
{
    struct clksync_result *r = &g_res[which];
    int ok = run(r);
    if (g_force == CLKSYNC_FORCE_PMTIMER) {
        ok = forced_reject(ok, r);           /* 開機或再驗證:不是 pmtimer 就不合格 */
    } else if (which == 0 && g_force != CLKSYNC_FORCE_NONE && r->reason != CLKSYNC_R_NO_COUNTER) {
        /* 測試:真正的驗證照做、照印,再把結果改成不合格,讓切換 / 降級路徑真的被執行 */
        ok = 0;
        r->ok = 0;
        r->reason = CLKSYNC_R_FORCED;
        r->bad_cpu = -1;
    }
    __atomic_store_n(&g_have[which], 1, __ATOMIC_SEQ_CST);
    print_result(which, r);
    if (!ok) {
        failed(which, r);
        return;
    }
    g_ok_at = r->at;
    __atomic_store_n(&g_state, CLKSYNC_OK, __ATOMIC_SEQ_CST);
    kprintf("clksync: check %s ok: %d cpus share counter %s (tolerance %llu counts = 1/4 tick)\n",
            check_name[which], r->ncpu, r->src, (unsigned long long)r->tol);
}

void clksync_boot_check(int ncpu)
{
    if (ncpu <= 1) {
        __atomic_store_n(&g_state, CLKSYNC_SINGLE, __ATOMIC_SEQ_CST);
        return;
    }
    live(0);
}

void clksync_recheck(void)
{
    static int done;

    if (__atomic_load_n(&g_state, __ATOMIC_SEQ_CST) != CLKSYNC_OK || done)
        return;                                  /* 單核、已降級、或已經再驗證過 */
    done = 1;
    /* 距離最近一次合格驗證至少 1 秒(timer_clock_hz() 個計數):用比較,不用除法。
       g_ok_at 是目前來源的單位(換過來源時是備援驗證的起點),不會和舊來源混用 */
    uint64_t hz = timer_clock_hz();
    while (read_count() - g_ok_at < hz)
        thread_sleep(10);
    live(1);
}

int clksync_state(void)
{
    return __atomic_load_n(&g_state, __ATOMIC_SEQ_CST);
}

const struct clksync_result *clksync_last(int which)
{
    if (which < 0 || which > 2 || !__atomic_load_n(&g_have[which], __ATOMIC_SEQ_CST))
        return NULL;
    return &g_res[which];
}

int clksync_fallback_used(void)
{
    return __atomic_load_n(&g_fallback, __ATOMIC_SEQ_CST);
}

void clksync_force(int mode)
{
    g_force = mode;
}

int clksync_run_dry(struct clksync_result *out)
{
    return run(out);
}

void clksync_test_inject(int cpu, uint64_t offset, uint64_t delay)
{
    __atomic_store_n(&sx_inj_cpu, -1, __ATOMIC_SEQ_CST);    /* 先關掉,再改數值,最後才指定對象 */
    sx_inj_off = offset;
    sx_inj_delay = delay;
    if (cpu >= 0)
        __atomic_store_n(&sx_inj_cpu, cpu, __ATOMIC_SEQ_CST);
}
