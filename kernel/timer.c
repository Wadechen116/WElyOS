/*
 * timer.c - 系統時鐘(與架構無關)
 *
 * 架構層讓每顆 CPU 的硬體計時器每秒產生 hz 次中斷,每次中斷呼叫 timer_tick()。
 * tick 計數是 kernel 的時間基準,之後的排程器也靠它切換工作。
 */
#include "arch.h"
#include "irq_late.h"
#include "cpu.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "watchdog.h"

/*
 * 系統時間(I-337):任何一顆 CPU 都可以推進,不依賴第 0 顆。
 *
 * 真實時間的來源是所有 CPU 共用的硬體計數器(arch_clock_count:i386 TSC、RISC-V time CSR、
 * AArch64 cntpct_el0)。g_deadline 是「下一個 tick 應該發生的計數值」。每顆 CPU 自己的計時器中斷
 * 都看一次:計數已經超過 g_deadline,就用 trylock 搶推進權;搶到的 CPU 把 tick 補到現在,
 * 並負責這幾個 tick 的全系統工作(喚醒休眠者、防飢餓、即時工作的期限,見 sched_tick_global)。
 * 沒搶到的不等待(另一顆 CPU 正在做同一件事)。所以第 0 顆 CPU 關中斷或卡住時,
 * 其他 CPU 照樣推進時間、叫醒休眠者。
 *
 * 速度:沒到期時每個 tick 只多一次共用快取列的讀取與一次計數器讀取;到期時每個 tick 只有
 * 「最先到的那顆」做一次原子操作成功,其他頂多一次失敗的 trylock,不自旋。
 * 安全:ticks 與 g_deadline 只在 time_lk 之內改,而且只會變大(單調);讀取端用序號
 * (seqcount)拿到一致的一對,i386 上 64 位元不會讀到一半。
 *
 * 沒有共用計數器時(i386 沒有 TSC 也沒有 HPET:g_iv == 0)退回舊做法:只有第 0 顆 CPU 推進,每次中斷 1 個 tick。
 *
 * 換時鐘來源(I-341):首選來源(i386 TSC)驗證不合格時,timer_clock_fallback() 切到備援(i386 HPET)。
 * 新舊來源的計數單位不同,所以「切換來源、重設 g_iv 與 g_deadline」整段在 time_lk 之內、序號為奇數的
 * 區間裡完成;需要「計數器現值 + (deadline, iv)」一致的讀者(time_snap)在同一個序號區間內讀計數器,
 * 跨過切換就重讀。持鎖推進的 CPU 在鎖內重讀一切,所以也不會混用。
 */
static volatile uint64_t ticks;
static volatile uint64_t g_deadline;   /* 下一個 tick 的計數值;g_iv == 0 時不用 */
/* 一個 tick 相當於多少計數;0 = 沒有共用計數器。timer_init 寫一次;之後只有 timer_clock_fallback 會改
   (持 time_lk、序號為奇數時)。讀者以 time_snap 在序號保護下讀(i386 上 64 位元不會讀到一半;
   64 位元的 __atomic 會需要 libatomic,所以不用) */
static volatile uint64_t g_iv;
/* 共用計數器路徑是否啟用(I-337 同步驗證)。開機時為 1:smp_init 之前只有第 0 顆 CPU 有計時器中斷,
   用它自己的計數器推進不會有跨 CPU 的問題。kernel/clksync.c 驗證任何一顆 AP 失敗時呼叫
   timer_shared_disable() 清成 0,之後退回「只有第 0 顆推進」的舊路徑(降級狀態)。只會由 1 變 0 */
static volatile int g_shared = 1;
static volatile uint32_t time_seq;     /* 奇數 = 寫入中 */
static struct spinlock time_lk = SPINLOCK_INIT_PROBED("timer-time", LK_TIME, 2);   /* probe 2 = lockhold_model.h 的 LH_TIME */
static uint32_t adv_count[MAX_CPUS];   /* 每顆 CPU 推進過幾次系統時間(量測與測試用;time_lk 之內寫) */
static unsigned tick_hz;

/* 一次最多補幾個 tick:超過時(整台機器被暫停,例如除錯器)放棄補記、重新對時 */
#define REAL_CATCHUP_MAX 100

static void time_read(uint64_t *t, uint64_t *d)
{
    uint32_t s;
    do {
        s = __atomic_load_n(&time_seq, __ATOMIC_ACQUIRE);
        *t = ticks;
        *d = g_deadline;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&time_seq, __ATOMIC_RELAXED));
}

/* 一致的 (tick, deadline, iv) 與「同一個區間內」讀到的計數器現值:跨過換來源就重讀(I-341) */
static void time_snap(uint64_t *t, uint64_t *d, uint64_t *iv, uint64_t *now)
{
    uint32_t s;
    do {
        s = __atomic_load_n(&time_seq, __ATOMIC_ACQUIRE);
        *t = ticks;
        *d = g_deadline;
        *iv = g_iv;
        *now = arch_clock_count();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&time_seq, __ATOMIC_RELAXED));
}

/* 呼叫時持有 time_lk */
static void time_write(uint64_t t, uint64_t d)
{
    __atomic_store_n(&time_seq, time_seq + 1, __ATOMIC_RELAXED);
    /* 完整屏障(Codex 209):「寫入中」的奇數標記必須先於下面的資料寫入被其他 CPU 看到。
       release 屏障只保證「之前的」存取不會被排到之後,擋不住後面的資料寫入跑到奇數標記前面
       (aarch64 / riscv64 是弱記憶體序) */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ticks = t;
    g_deadline = d;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);     /* 資料寫完,才公布偶數(寫完) */
    __atomic_store_n(&time_seq, time_seq + 1, __ATOMIC_RELAXED);
}

void timer_init(unsigned hz)
{
    tick_hz = hz;
    arch_timer_init(hz);
    uint64_t iv = arch_tick_interval();
    if (iv && arch_clock_hz()) {
        g_deadline = arch_clock_count() + iv;
        g_iv = iv;
    }
}

/*
 * 試著把系統時間推進到現在。成功(真的推進了)回傳 1,此時仍持有 time_lk,
 * *from、*to 是推進前後的 tick;呼叫端做完這段的全系統工作後放開 time_lk。
 */
/* 目前生效的每 tick 計數:共用路徑停用時視同沒有共用計數器 */
static uint64_t live_iv(void)
{
    uint64_t t, d, iv, now;
    time_snap(&t, &d, &iv, &now);            /* g_iv 可能被換來源改寫:在序號保護下讀 */
    return __atomic_load_n(&g_shared, __ATOMIC_ACQUIRE) ? iv : 0;
}

/*
 * I-346 降級的保證(Codex msg 20261123-002 的方案 A):沒有可用的共用計數器時,系統時間只隨第 0 顆
 * 「真的送達」的計時器中斷逐次 +1,架構層補記的 tick(timer_tick_stolen)一律丟掉、只記數。
 * 原因:降級時任何 CPU 都無法往前看「已經到了、還沒記上」的 tick(timer_ticks_real 只能回傳 t),
 * 若恢復時批次補記,落後期間取的期限會提早到期(sleep 承諾「至少」被打破)。
 * 代價(明訂、不是缺陷):時間仍單調,但會落後實際經過的時間;睡眠、即時工作的期限檢查、
 * 防飢餓都相應延後;由 tick 推算的日曆時間也會落後。落後量見 timer_stats 的 degraded_*。
 * 正常(共用計數器)模式不受影響
 */
static uint32_t deg_dropped, deg_run, deg_max_run;   /* 只有第 0 顆在中斷中寫;讀取用 atomic */

/*
 * I-349 校準(只給自我測試,開機參數 stallinj=steady;Codex 020 / 021 同意的延遲點):推進時間的 CPU
 * 在拿到 time_lk 之後、提交 tick 之前,每 10 個 tick 忙等 3 個 tick 的計數。拿著鎖延遲,別顆的 trylock
 * 會失敗、交給它,所以這確實拖住時間的推進,不會被別顆繞過。量時間只讀硬體計數器(arch_clock_count),
 * 不碰 time_lk。這模擬的是「kernel 內部的推進延遲」,不是主機不給虛擬 CPU 時間。
 * 只有 inj_active(由測試在 CPU0 卡住期間打開)時才作用;正式路徑的成本是一次讀取。
 */
static volatile int inj_steady, inj_active;
static uint64_t inj_last;                    /* 上次注入的計數值;time_lk 之內讀寫 */
static volatile uint32_t inj_count, inj_cpus;
static volatile uint64_t inj_total, inj_max;

void timer_test_steady(int enable)
{
    inj_steady = enable;
}

void timer_test_steady_window(int active)
{
    if (active) {
        inj_count = inj_cpus = 0;
        inj_total = inj_max = 0;
        inj_last = 0;
    }
    __atomic_store_n(&inj_active, active, __ATOMIC_SEQ_CST);
}

void timer_test_steady_stats(uint32_t *count, uint32_t *cpus, uint64_t *total, uint64_t *max)
{
    *count = inj_count;
    *cpus = inj_cpus;
    *total = inj_total;
    *max = inj_max;
}

/* 呼叫時持有 time_lk;iv = 每 tick 計數 */
static void inject_steady_delay(uint64_t iv)
{
    uint64_t now = arch_clock_count();
    if (inj_last && now - inj_last < 10 * iv)
        return;
    uint64_t t0 = now;
    while (arch_clock_count() - t0 < 3 * iv)
        arch_cpu_relax();
    uint64_t d = arch_clock_count() - t0;
    inj_last = arch_clock_count();
    inj_count++;
    inj_cpus |= 1u << this_cpu()->id;
    inj_total += d;
    if (d > inj_max)
        inj_max = d;
}

static int timer_advance(uint64_t *from, uint64_t *to, int charge)
{
    uint64_t t, d, iv, now;

    time_snap(&t, &d, &iv, &now);
    if (!__atomic_load_n(&g_shared, __ATOMIC_ACQUIRE) || iv == 0) {   /* 沒有可用的共用計數器:舊做法 */
        if (this_cpu()->id != 0)
            return 0;
        if (!charge) {                        /* 補記的 tick:丟掉(見上方 I-346) */
            __atomic_store_n(&deg_dropped, deg_dropped + 1, __ATOMIC_RELAXED);
            deg_run++;
            if (deg_run > deg_max_run)
                __atomic_store_n(&deg_max_run, deg_run, __ATOMIC_RELAXED);
            return 0;
        }
        deg_run = 0;
        if (!spin_trylock(&time_lk))
            return 0;
        time_read(&t, &d);
        time_write(t + 1, d);
        *from = t;
        *to = t + 1;
        adv_count[0]++;
        return 1;
    }
    if (now < d)                              /* 常見情形:還沒到,什麼都不做 */
        return 0;
    if (!spin_trylock(&time_lk))              /* 別顆 CPU 正在推進:交給它 */
        return 0;
    /* 拿到鎖之後重讀:可能剛被推進過,也可能剛換了來源(換來源只在鎖內做,所以鎖內讀到的一定一致) */
    if (!__atomic_load_n(&g_shared, __ATOMIC_ACQUIRE)) {
        spin_unlock(&time_lk);                /* 剛被降級:這次不推進,下一次中斷走舊做法 */
        return 0;
    }
    time_read(&t, &d);
    iv = g_iv;
    if (inj_steady && __atomic_load_n(&inj_active, __ATOMIC_ACQUIRE))
        inject_steady_delay(iv);             /* I-349 校準(只在自我測試、參數 stallinj=steady 時) */
    now = arch_clock_count();
    unsigned n = 0;
    while (d <= now && n < REAL_CATCHUP_MAX) {
        d += iv;
        n++;
    }
    if (d <= now)                             /* 落後太多:放棄補記、重新對時(與架構層相同) */
        d = now + iv;
    if (n == 0) {                             /* 別顆 CPU 剛推進過 */
        spin_unlock(&time_lk);
        return 0;
    }
    time_write(t + n, d);
    adv_count[this_cpu()->id]++;
    *from = t;
    *to = t + n;
    return 1;
}

static void timer_common(int charge)
{
    uint64_t from, to;
    if (timer_advance(&from, &to, charge)) {
        sched_tick_global(from, to);          /* 這段 tick 的全系統工作只由推進的這顆 CPU 做一次 */
        spin_unlock(&time_lk);
    }
    watchdog_tick();           /* 心跳,並看看隔壁的 CPU 是不是還活著 */
    sched_tick(charge);        /* 這顆 CPU 自己的時間片(排程器未啟動時無作用) */
}

void timer_tick(void)
{
    timer_common(1);
}

void timer_tick_stolen(void)
{
    timer_common(0);           /* 只推進時間:不記 CPU 時間、不扣時間片與即時工作的預算 */
}

uint64_t timer_ticks(void)
{
    uint64_t t, d;
    time_read(&t, &d);
    return t;
}

/*
 * 停用共用計數器路徑(I-337 同步驗證失敗時,由 kernel/clksync.c 呼叫)。之後只有第 0 顆 CPU
 * 每次中斷推進 1 個 tick。ticks 只增不減:正在推進的 CPU 會照常做完它那一次(在 time_lk 之內),
 * 之後的推進全走舊路徑,所以時間仍單調。不可逆:降級後不會自己恢復。
 */
void timer_shared_disable(void)
{
    __atomic_store_n(&g_shared, 0, __ATOMIC_RELEASE);
    /* I-351:降級後共用計數器不再可信,所有線上 CPU 的晚到觀測一併停用(iv = 0) */
    for (int i = 0; i < MAX_CPUS; i++)
        if (cpus[i].online)
            arch_irq_late_config_cpu(i, 0);
}

int timer_shared_enabled(void)
{
    return live_iv() != 0;
}

uint64_t timer_tick_counts(void)
{
    return g_iv;
}

uint32_t timer_advances(int cpu)
{
    return (cpu >= 0 && cpu < MAX_CPUS) ? __atomic_load_n(&adv_count[cpu], __ATOMIC_RELAXED) : 0;
}

/* 晚到的紀錄(見 timer.h)。中斷處理程式(各 CPU 都可能)寫入,以一把 spinlock 保護;只有幾條指令 */
static struct timer_stats tstats;
static struct spinlock tstat_lk = SPINLOCK_INIT("timer-stats", LK_TSTAT);
static const unsigned bucket_lo[TIMER_BURST_BUCKETS] = { 2, 3, 4, 6, 7, 10 };

void timer_note_catchup(int n, int resynced)
{
    if (n < 2 && !resynced)
        return;
    unsigned b = TIMER_BURST_BUCKETS - 1;
    if (!resynced)
        while (b > 0 && (unsigned)n < bucket_lo[b])
            b--;
    unsigned long fl = spin_lock_irqsave(&tstat_lk);
    tstats.burst[b]++;
    if (resynced)
        tstats.resyncs++;
    else if ((uint32_t)n > tstats.max_burst)
        tstats.max_burst = (uint32_t)n;
    spin_unlock_irqrestore(&tstat_lk, fl);
}

void timer_get_stats(struct timer_stats *st)
{
    unsigned long fl = spin_lock_irqsave(&tstat_lk);
    *st = tstats;
    spin_unlock_irqrestore(&tstat_lk, fl);
    st->degraded_dropped = __atomic_load_n(&deg_dropped, __ATOMIC_RELAXED);
    st->degraded_max_run = __atomic_load_n(&deg_max_run, __ATOMIC_RELAXED);
}

int timer_diag_peek(uint64_t *t, struct timer_stats *st)
{
    int ok = 0;
    for (int i = 0; i < 1000; i++) {         /* 有限次:寫入端可能正停在序號為奇數的區間裡(就是卡住的那一顆) */
        uint32_t s = __atomic_load_n(&time_seq, __ATOMIC_ACQUIRE);
        uint64_t v = ticks;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(s & 1) && s == __atomic_load_n(&time_seq, __ATOMIC_RELAXED)) {
            *t = v;
            ok |= 1;
            break;
        }
    }
    if (spin_trylock(&tstat_lk)) {           /* 只試一次:鎖可能正被卡住的 CPU 持有 */
        *st = tstats;
        spin_unlock(&tstat_lk);
        st->degraded_dropped = __atomic_load_n(&deg_dropped, __ATOMIC_RELAXED);
        st->degraded_max_run = __atomic_load_n(&deg_max_run, __ATOMIC_RELAXED);
        ok |= 2;
    }
    return ok;
}

uint32_t timer_bursts_at_least(const struct timer_stats *st, unsigned k)
{
    uint32_t sum = 0;

    for (unsigned b = 0; b < TIMER_BURST_BUCKETS; b++)
        if (bucket_lo[b] >= k)
            sum += st->burst[b];
    return sum;
}

unsigned timer_hz(void)
{
    return tick_hz;
}

/*
 * ---- 實際的日期時間 ----
 *
 * tick 只知道「開機之後過了多久」。要知道「現在是幾月幾號幾點」,得問硬體的即時時鐘
 * (RTC):主機板上一個有電池的小時鐘,關機時也在走。讀它比較慢(i386 要好幾次 I/O),
 * 所以只在開機時問一次,記下「開機那一刻是第幾秒」,之後的時間 = 那一刻 + 開機後經過的時間。
 */
static uint64_t boot_epoch;                  /* 開機那一刻的 Unix 時間(秒) */

/* 64 位元除以 32 位元。不直接寫 n / d:i386 上那需要編譯器的輔助函式庫(見 sched.c 的說明)。
   每次處理 16 個位元,被除數因此放得進 64 位元的一半;d 不可超過 65535 */
static uint64_t div_u64(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint64_t q = 0;
    uint32_t r = 0;

    for (int shift = 48; shift >= 0; shift -= 16) {
        uint32_t part = (r << 16) | (uint32_t)((n >> shift) & 0xFFFF);
        q |= (uint64_t)(part / d) << shift;
        r = part % d;
    }
    if (rem)
        *rem = r;
    return q;
}

void timer_clock_init(void)
{
    uint64_t now = arch_rtc_read();
    uint64_t up = div_u64(timer_ticks(), tick_hz ? tick_hz : 1, NULL);

    boot_epoch = now > up ? now - up : 0;
}

uint64_t timer_epoch(uint32_t *usec)
{
    uint32_t rem = 0;
    uint64_t sec = div_u64(timer_ticks(), tick_hz ? tick_hz : 1, &rem);

    if (usec)
        *usec = tick_hz ? rem * (1000000u / tick_hz) : 0;
    return boot_epoch + sec;
}

/* ---- 開機時間的記號 ---- */
static uint64_t mark_kmain, mark_ready;

void timer_clock_mark(int which)
{
    uint64_t *m = which == CLOCK_MARK_KMAIN ? &mark_kmain : &mark_ready;
    if (*m == 0)
        *m = arch_clock_count();
}

void timer_clock_marks(uint64_t *kmain, uint64_t *ready)
{
    *kmain = mark_kmain;
    *ready = mark_ready;
}

uint64_t timer_clock_count(void)
{
    return arch_clock_count();
}

/*
 * 換到備援的共用計數器(I-341;由 kernel/clksync.c 在首選來源驗證不合格時呼叫)。
 * 回傳 1 = 已換(呼叫端接著必須驗證新來源);0 = 沒有備援。
 * 整段在 time_lk 之內、序號為奇數時做:換來源、g_iv 換成新單位、g_deadline 以新來源的現值重設。
 * ticks 不變(時間單調;換的那一刻最多少記不到一個 tick 的零頭)。開機標記是舊單位,清成 0(= 不知道)。
 */
int timer_clock_fallback(void)
{
    unsigned long fl = spin_lock_irqsave(&time_lk);
    __atomic_store_n(&time_seq, time_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int ok = arch_clock_fallback();
    if (ok) {
        uint64_t iv = arch_tick_interval();
        g_iv = iv;
        g_deadline = arch_clock_count() + iv;
        mark_kmain = mark_ready = 0;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&time_seq, time_seq + 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&time_lk, fl);
    if (ok && (g_iv == 0 || arch_clock_hz() == 0))
        timer_shared_disable();              /* 不應發生(arch 層保證);萬一發生,寧可降級也不用 0 當間隔 */
    return ok && timer_shared_enabled();
}

const char *timer_clock_name(void)
{
    return arch_clock_name();
}

uint64_t timer_clock_hz(void)
{
    return arch_clock_hz();
}

uint64_t timer_ticks_real(void)
{
    /* 一致的一對(tick, 下一個 tick 的計數值),再看計數器已經超過幾個預定時間:
       那些是「已經到了、還沒有任何 CPU 記上」的 tick */
    uint64_t t, d, iv, now;
    time_snap(&t, &d, &iv, &now);
    if (!__atomic_load_n(&g_shared, __ATOMIC_ACQUIRE) || iv == 0)
        return t;
    unsigned k = 0;
    while (now >= d && k < REAL_CATCHUP_MAX) {
        d += iv;
        k++;
    }
    return t + k;
}

uint64_t timer_wait_base(void)
{
    /* 降級時第 0 顆關中斷期間 tick 停住;開中斷後送達的第一個中斷照常 +1,但那一格的時間其實已經過了。
       其餘補記的都丟掉(timer_advance),所以晚到的最多就這一格 */
    return timer_ticks_real() + (timer_shared_enabled() ? 0 : 1);
}

uint64_t timer_deadline_ms(unsigned ms)
{
    if (ms == 0)
        return 0;
    /* 現在位在第 r 個 tick 之中的某處;到第 r + n 個 tick 時,真實經過的時間至少 n - 1 個 tick,
       所以 n = 無條件進位的 tick 數 + 1(與 thread_sleep 相同;32 位元運算,避開 i386 的 __udivdi3) */
    unsigned n = (ms * timer_hz() + 999) / 1000 + 1;
    return timer_wait_base() + n;
}

int timer_expired(uint64_t deadline)
{
    return timer_ticks() >= deadline;
}
