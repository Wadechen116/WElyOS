/*
 * Host model for sched.c's fixed-queue starvation scan.
 * A pinned thread is front-requeued only when the running thread on that CPU
 * is preempted with slice remaining. Ordinary wakeups and exhausted slices
 * go to the tail. Model the front-requeue case as one runnable head which is
 * repeatedly selected and preempted, plus an older queue behind it.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#define PIN_SCAN_MAX 24u
#define STARVE_TICKS 100u
#define STARVE_SCAN_TICKS 10u
#define NODE_CAP 128u
#define CURSOR_NODE_CAP 20000u

struct node {
    unsigned long ready_since;
    int boosted;
    struct node *next;
};

static struct node nodes[NODE_CAP];
static size_t used;

struct cursor_node {
    unsigned long ready_since;
    int boosted;
    struct cursor_node *next, *prev;
};

struct cursor_queue {
    struct cursor_node *head, *tail, *scan;
};

static struct cursor_node cursor_nodes[CURSOR_NODE_CAP];
static size_t cursor_used;

static struct cursor_node *new_cursor_node(unsigned long ready_since)
{
    assert(cursor_used < CURSOR_NODE_CAP);
    struct cursor_node *n = &cursor_nodes[cursor_used++];
    n->ready_since = ready_since;
    n->boosted = 0;
    n->next = n->prev = NULL;
    return n;
}

static void cursor_push_front(struct cursor_queue *q, struct cursor_node *n)
{
    n->prev = NULL;
    n->next = q->head;
    if (q->head)
        q->head->prev = n;
    else
        q->tail = n;
    q->head = n;
}

static void cursor_push_tail(struct cursor_queue *q, struct cursor_node *n)
{
    n->next = NULL;
    n->prev = q->tail;
    if (q->tail)
        q->tail->next = n;
    else
        q->head = n;
    q->tail = n;
}

/* Model pin_unlink(), including moving a cursor away from the removed node. */
static void cursor_unlink(struct cursor_queue *q, struct cursor_node *n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        q->head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        q->tail = n->prev;
    if (q->scan == n)
        q->scan = n->next;
    n->next = n->prev = NULL;
}

static void cursor_scan(struct cursor_queue *q, unsigned long now, int rotating)
{
    struct cursor_node *n = rotating && q->scan ? q->scan : q->head;
    struct cursor_node *next = NULL;

    for (unsigned k = 0; n && k < PIN_SCAN_MAX; ++k, n = next) {
        next = n->next;
        if (n->boosted || now < n->ready_since ||
            now - n->ready_since < STARVE_TICKS)
            continue;
        n->boosted = 1;
        cursor_unlink(q, n); /* update_prio() removes the boosted node from this layer */
    }
    if (rotating)
        q->scan = n;
}

static long run_cursor_case(int rotating, unsigned old_count, unsigned long max_tick)
{
    struct cursor_queue q = {0};
    struct cursor_node *target = NULL;

    cursor_used = 0;
    for (unsigned i = 0; i < old_count; ++i) {
        struct cursor_node *n = new_cursor_node(0);
        cursor_push_tail(&q, n);
        target = n;
    }

    for (unsigned long now = 1; now <= max_tick; ++now) {
        /* Adversarial stress: one fresh front insertion on every tick. */
        cursor_push_front(&q, new_cursor_node(now));
        if (now % STARVE_SCAN_TICKS == 0)
            cursor_scan(&q, now, rotating);
        if (target->boosted)
            return (long)now;
    }
    return -1;
}

static struct node *new_node(unsigned long ready_since)
{
    assert(used < NODE_CAP);
    struct node *n = &nodes[used++];
    n->ready_since = ready_since;
    n->boosted = 0;
    n->next = NULL;
    return n;
}

static void push_front(struct node **head, struct node *n)
{
    n->next = *head;
    *head = n;
}

static void push_back(struct node **head, struct node *n)
{
    while (*head)
        head = &(*head)->next;
    *head = n;
}

static void unlink_node(struct node **head, struct node *target)
{
    while (*head && *head != target)
        head = &(*head)->next;
    assert(*head == target);
    *head = target->next;
    target->next = NULL;
}

/* Emulate schedule(): the same running node returns at the front. */
static void preempt_front(struct node **head, struct node *hot,
                          unsigned long now)
{
    unlink_node(head, hot);
    hot->ready_since = now;
    push_front(head, hot);
}

/* Boosted nodes leave this priority layer, as update_prio() does. */
static size_t scan(struct node **head, unsigned long now, size_t limit)
{
    size_t seen = 0, promoted = 0;
    struct node **link = head;

    while (*link && seen < limit) {
        struct node *n = *link;
        ++seen;
        if (!n->boosted && now >= n->ready_since &&
            now - n->ready_since >= STARVE_TICKS) {
            *link = n->next;
            n->next = NULL;
            n->boosted = 1;
            ++promoted;
        } else {
            link = &n->next;
        }
    }
    return promoted;
}

static struct node *at_position(struct node *head, size_t position)
{
    while (head && position > 1) {
        head = head->next;
        --position;
    }
    return head;
}

int main(void)
{
    struct node *head = NULL;
    struct node *hot = new_node(0);
    struct node *target = new_node(0);
    push_back(&head, target);
    push_front(&head, hot);

    /* Head-only scanning can miss the old waiter behind a repeatedly fresh head. */
    preempt_front(&head, hot, STARVE_TICKS);
    assert(at_position(head, 2) == target);
    assert(scan(&head, STARVE_TICKS, 1) == 0);
    assert(!target->boosted);

    /* The real 24-node scan reaches that same position-2 waiter. */
    assert(scan(&head, STARVE_TICKS, PIN_SCAN_MAX) == 1);
    assert(target->boosted);

    /* A finite old backlog is removed in batches of at most 24 per scan. */
    used = 0;
    head = NULL;
    hot = new_node(0);
    target = new_node(0);
    push_back(&head, target);
    for (size_t i = 0; i < PIN_SCAN_MAX * 2; ++i)
        push_front(&head, new_node(0));
    push_front(&head, hot);

    size_t rounds = 0;
    while (!target->boosted && rounds < 4) {
        ++rounds;
        preempt_front(&head, hot, STARVE_TICKS + rounds);
        (void)scan(&head, STARVE_TICKS + rounds, PIN_SCAN_MAX);
    }
    assert(target->boosted);
    assert(rounds <= 3); /* one hot head consumes a slot, leaving 23 old nodes/round */

    /* Normal ready insertion is at the tail; it cannot continually cover old nodes. */
    struct node *fresh = new_node(STARVE_TICKS + rounds);
    push_back(&head, fresh);
    assert(scan(&head, STARVE_TICKS + rounds, PIN_SCAN_MAX) == 0);

    /* A rotating cursor reaches old nodes despite repeated fresh front inserts. */
    for (unsigned old_count = 1; old_count <= 200; old_count += 7) {
        long head_scan_tick = run_cursor_case(0, old_count, 5000);
        long cursor_scan_tick = run_cursor_case(1, old_count, 5000);
        assert(head_scan_tick < 0);
        assert(cursor_scan_tick >= (long)STARVE_TICKS);
        /* Use a generous bound for scan cadence plus a growing queue. */
        assert((unsigned long)cursor_scan_tick <=
               STARVE_TICKS + STARVE_SCAN_TICKS * (old_count / (PIN_SCAN_MAX - STARVE_SCAN_TICKS) + 4));
    }

    /* Removing/migrating the node at the cursor advances it to a live successor. */
    struct cursor_queue source = {0}, destination = {0};
    struct cursor_node *a = new_cursor_node(0), *b = new_cursor_node(0), *c = new_cursor_node(0);
    cursor_push_tail(&source, a);
    cursor_push_tail(&source, b);
    cursor_push_tail(&source, c);
    source.scan = b;
    cursor_unlink(&source, b);
    assert(source.scan == c);
    cursor_push_tail(&destination, b); /* affinity migration to another queue */
    assert(source.scan != b && destination.head == b);

    source.scan = c;
    cursor_unlink(&source, c); /* removing the last cursor node wraps to NULL */
    assert(source.scan == NULL && source.tail == a);

    puts("PASS: bounded scan handles the actual single-head case; rotating cursor also reaches old waiters under repeated fresh front inserts");
    return 0;
}
