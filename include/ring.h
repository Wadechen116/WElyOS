#ifndef WELYOS_RING_H
#define WELYOS_RING_H
#include <stdint.h>

/*
 * 無鎖環狀佇列(lock-free circular queue),先進先出。
 *
 *   - 放入(ring_push)只能由「擁有者」做:同一時間只有一個放入者
 *   - 取出(ring_take)任何人都可以做,可以多個同時進行:擁有者取自己的工作,
 *     別顆 CPU 來「竊取」,走的是同一個函式
 * 這種「單一放入者、多個取出者」的組合稱為 SPMC(Single Producer Multiple Consumer)。
 * Go 語言排程器每顆 CPU 的本地佇列就是這個結構。
 *
 * 結構:一個固定大小的陣列,加上兩個只會增加的計數器
 *
 *        head                 tail
 *         v                    v
 *   [ ][ ][A][B][C][D][ ][ ][ ]        索引 = 計數器 % 大小,用到尾端就繞回開頭
 *
 *   tail:下一個要放入的位置。只有擁有者會寫
 *   head:下一個要取出的位置。取出者們用 CAS(Compare-And-Swap,比較並交換)競爭:
 *         「如果 head 還是我剛才看到的值,就把它加一」。成功的人拿走那一格;
 *         失敗代表被別人搶先了,重新再來。
 *
 * 為什麼不需要鎖:
 *   - 放入者只寫 tail 與「tail 指到的那一格」,而那一格在 tail 前進之前沒有人會去讀
 *   - 取出者先讀格子的內容、再用 CAS 確認 head 沒變。那一格要被新的資料覆蓋,
 *     head 必須先越過它;所以只要 CAS 成功,就保證剛才讀到的內容是有效的。
 *     CAS 失敗時讀到的內容直接丟棄
 *   - 計數器只增不減(32 位元,繞一圈要 40 億次操作),所以不會發生
 *     「值變了又變回來,CAS 誤以為沒變」的 ABA 問題
 *
 * 沒有鎖的好處:不會因為持有鎖的一方被延遲而讓所有人等;擁有者放入與取出
 * 在沒有人來竊取時幾乎只是幾次記憶體讀寫。
 * 代價:容量固定(滿了要另外處理)、不能從中間移除、只能放指標大小的東西。
 */
#define RING_SIZE 64u                        /* 必須是 2 的次方 */
#define RING_MASK (RING_SIZE - 1)

struct ring {
    volatile uint32_t head;                  /* 下一個要取出的 */
    volatile uint32_t tail;                  /* 下一個要放入的 */
    void *volatile slot[RING_SIZE];
};

/* 放入(僅擁有者)。滿了回傳 0 */
static inline int ring_push(struct ring *r, void *p)
{
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);    /* 只有自己會寫 tail */
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

    if (t - h >= RING_SIZE)                  /* 無號數相減:計數器繞回也正確 */
        return 0;
    __atomic_store_n(&r->slot[t & RING_MASK], p, __ATOMIC_RELAXED);
    /* release:保證別人看到新的 tail 時,上面那一格的內容也已經看得到 */
    __atomic_store_n(&r->tail, t + 1, __ATOMIC_RELEASE);
    return 1;
}

/* 取出最早放入的一個(任何人)。空的回傳 NULL */
static inline void *ring_take(struct ring *r)
{
    for (;;) {
        uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

        if (h == t)
            return NULL;
        void *p = __atomic_load_n(&r->slot[h & RING_MASK], __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(&r->head, &h, h + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return p;
        /* 被別人搶先:重來 */
    }
}

/* 取出最早放入的一個,並告訴呼叫者它是第幾個放入的(*seq = 當時的 head 計數器)。
   同一個環裡,序號在「這一格被取走」之前不會被別的內容再用到:環最多 RING_SIZE 格,
   tail 不可能超過 head + RING_SIZE,所以序號相同就代表是同一次放入 */
static inline void *ring_take_seq(struct ring *r, uint32_t *seq)
{
    for (;;) {
        uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        if (h == t)
            return NULL;
        void *p = __atomic_load_n(&r->slot[h & RING_MASK], __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(&r->head, &h, h + 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *seq = h;
            return p;
        }
    }
}

/* 下一個放入的會拿到的序號(僅擁有者;和 ring_push 搭配,先記下再放入) */
static inline uint32_t ring_next_seq(struct ring *r)
{
    return __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
}

/* 偷看最早放入的一個,不取出。結果只是參考(回傳時可能已經被取走) */
static inline void *ring_peek(struct ring *r)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

    return h == t ? NULL : __atomic_load_n(&r->slot[h & RING_MASK], __ATOMIC_RELAXED);
}

/* 如果最早放入的那一個正好是 p,就把它丟掉(任何人)。用來清除已知無效的項目。
   回傳 1 表示丟掉了;0 表示頭已經不是它(被別人取走了) */
static inline int ring_drop_head_if(struct ring *r, void *p)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

    if (h == t || __atomic_load_n(&r->slot[h & RING_MASK], __ATOMIC_RELAXED) != p)
        return 0;
    return __atomic_compare_exchange_n(&r->head, &h, h + 1, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static inline int ring_empty(struct ring *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_RELAXED) ==
           __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
}
#endif
