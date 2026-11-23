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
#define NODE_CAP 128u

struct node {
    unsigned long ready_since;
    int boosted;
    struct node *next;
};

static struct node nodes[NODE_CAP];
static size_t used;

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

    puts("PASS: repeated front requeue creates one fresh head; scan reaches old waiters, including after a 48-node backlog");
    return 0;
}
