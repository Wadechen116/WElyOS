#ifndef WELYOS_RUNQ_H
#define WELYOS_RUNQ_H

/*
 * 執行佇列(run queue)的介面:存放「可以執行、正在等 CPU」的工作,並決定下一個輪到誰。
 *
 * 這一層刻意與排程器的其他部分分開,目的是「實作可以整個換掉」:
 *   - 介面只認得 struct rq_node(嵌在 struct thread 裡的一小塊),不認得 struct thread,
 *     所以實作不依賴排程器的內部結構,也能單獨拿到一般電腦上以多條執行緒測試
 *     (見 tests/host/)。
 *   - 上鎖由實作自己負責:所有函式都可以由任何 CPU 同時呼叫,呼叫者不必(也不該假設)
 *     持有任何鎖。唯一的要求是呼叫時中斷已關閉。
 *   - 排程策略(優先權怎麼算、何時搶佔、防飢餓)留在 sched.c,這裡只負責「存放與取出」。
 *
 * 目前有兩個實作,以 CMake 選項 WELYOS_RUNQ 選擇(見 docs/DEVLOG.md 第 16.1 步):
 *   ring  kernel/runq_ring.c  每顆 CPU 一組無鎖環狀佇列,閒置時向別顆 CPU 竊取(預設)
 *   list  kernel/runq_list.c  全系統共用一組串列,以一把鎖保護(最單純,當作對照與退路)
 * 要加第三種做法,只要再寫一個實作這七個函式的檔案。
 *
 * 語意(兩個實作都必須遵守):
 *   - 優先權 0 ~ RUNQ_LEVELS-1,數字大的先取出
 *   - 相同優先權大致先進先出(ring 在多顆 CPU 之間不保證嚴格的順序)
 *   - 一個節點在同一時間最多只會被 runq_pop 交給一個呼叫者
 */
#define RUNQ_LEVELS 16

struct rq_node {
    struct rq_node *next, *prev;     /* 串列用的連結(list;ring 的溢出串列) */
    volatile int prio;               /* 排在哪一個優先權 */
    volatile unsigned queued;        /* ring:1 = 在佇列中、尚未被取走(見 runq_ring.c) */
    volatile unsigned where;         /* ring:是否串在溢出串列上 */
    int ovf_prio;                    /* ring:串在哪一條溢出串列 */
};

void runq_init(void);
const char *runq_name(void);         /* 實作的名稱,開機時印出 */

/* 放入一個「不在佇列中」的節點。front = 1 表示它是被搶佔的、時間片還沒用完,
   同優先權中應該排在最前面 */
void runq_push(struct rq_node *n, int prio, int front);

/* 取出優先權最高的節點;沒有任何節點時回傳 NULL */
struct rq_node *runq_pop(void);

/* 一個「在佇列中」的節點改了優先權:讓它之後依新的優先權被取出 */
void runq_reprio(struct rq_node *n, int prio);

/* 目前佇列中最高的優先權;空的回傳 -1。只是參考值:別顆 CPU 可能同時在取放 */
int runq_highest(void);

/*
 * 列出優先權 prio 中「等最久」的候選者(各佇列的頭),最多 max 個,回傳個數。
 * 防飢餓檢查用。結果只是參考:回傳時那個節點可能已經被取走,
 * 呼叫者必須自行確認它的狀態;節點所在的記憶體不可以被釋放(見 sched.c 的 thread pool)。
 */
int runq_peek(int prio, struct rq_node **out, int max);
#define RUNQ_PEEK_MAX 24
#endif
