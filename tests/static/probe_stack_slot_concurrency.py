#!/usr/bin/env python3
"""Current stack-slot interleaving counterexample plus a rollback ownership model.

Production probe exit 0 means the current defect reproduced. The separate finite
model describes an acceptance contract; it is not a proposed product patch.
"""
from collections import deque
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from probe_pte_ticket import function

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define PROC_MAX_THREADS 16
#define PAGE_SIZE 4096UL
#define USER_STACK_TOP 0x70000000UL
#define USER_STACK_SLOT 0x100000UL
#define USER_STACK_PAGES 4u
#define VM_READ 1
#define VM_WRITE 2
struct vm_space { int unused; };
struct proc { unsigned nstacks; struct vm_space *space; };
static uintptr_t alloc_user_stack(struct proc *p);
static struct proc *target;
static uintptr_t nested_top;
static unsigned nested, collisions, total_maps;
static uintptr_t addresses[8];
static void *map_page(struct vm_space *s, uintptr_t a, unsigned f)
{
    (void)s; (void)f;
    /* Pause outer creator after it selected a slot but before first page exists.
       A second creator completes at that point, then the outer one resumes. */
    if (!nested) {
        nested = 1;
        nested_top = alloc_user_stack(target);
    }
    for (unsigned i = 0; i < total_maps; i++)
        if (addresses[i] == a)
            collisions++;
    addresses[total_maps++] = a;
    return (void *)1;
}
CODE
int main(void)
{
    struct proc p = { .nstacks = 1 };
    target = &p;
    uintptr_t outer_top = alloc_user_stack(&p);
    if (outer_top != nested_top || !outer_top || p.nstacks != 3 || collisions != 4)
        return 2;
    printf("REPRODUCED: two successful creators share stack top 0x%lx; "
           "nstacks=%u, four duplicate page mappings\n", (unsigned long)outer_top, p.nstacks);
    return 0;
}
'''


def ownership_model(mutation=None):
    """Two allocators, two reusable slots, two initial pages per slot.

    owner 0 = free, owners 1/2 = creators; state (owner,pages,live).
    Slot claiming, mapping publication, and rollback are atomic at model steps.
    """
    initial = ((0, 0, False), (0, 0, False))
    seen = {initial}
    queue = deque([initial])
    edges = 0
    while queue:
        slots = queue.popleft()
        owners = [s[0] for s in slots if s[0]]
        assert len(owners) == len(set(owners))
        for owner, pages, live in slots:
            assert owner != 0 or (pages == 0 and not live)
            assert not live or pages == 2
        for actor in (1, 2):
            held = next((i for i, s in enumerate(slots) if s[0] == actor), None)
            candidates = []
            if held is None:
                for i, slot in enumerate(slots):
                    if slot[0] == 0:
                        candidates.append((i, (actor, 0, False), 'claim'))
            else:
                _, pages, live = slots[held]
                if live:
                    candidates.append((held, (0, 0, False), 'exit'))
                else:
                    cleared = (0, pages, False) if mutation == 'leave-pages' else (0, 0, False)
                    candidates.append((held, cleared, 'failure-rollback'))
                    if pages < 2:
                        candidates.append((held, (actor, pages + 1, False), 'map'))
                    else:
                        candidates.append((held, (actor, pages, True), 'publish'))
            for i, value, event in candidates:
                next_slots = list(slots)
                next_slots[i] = value
                if mutation == 'release-other-owner' and event == 'failure-rollback':
                    for j, other in enumerate(slots):
                        if j != i and other[0]:
                            next_slots[j] = (0, 0, False)
                # A rollback/exit cannot affect another actor's slot/pages.
                for j in range(len(slots)):
                    if j != i:
                        assert next_slots[j] == slots[j]
                if event == 'failure-rollback':
                    assert next_slots[i] == (0, 0, False)
                state = tuple(next_slots)
                edges += 1
                if state not in seen:
                    seen.add(state)
                    queue.append(state)
    # Counterexample to an unowned high-water decrement rollback:
    # A owns slot1, B owns slot2; A fails and decrements from3 to2;
    # the next creator reuses still-live B's slot2.
    high_water = 1
    a = high_water; high_water += 1
    b = high_water; high_water += 1
    high_water -= 1
    c = high_water
    assert a == 1 and b == c == 2
    return dict(states=len(seen), transitions=edges,
                blind_decrement_counterexample=dict(a=a, b=b, retry=c))


def main():
    paths = ['kernel/proc.c', 'kernel/proc_internal.h']
    inputs = {p: (ROOT / p).read_text() for p in paths}
    source = inputs['kernel/proc.c']
    code = '\n'.join(function(source, n) for n in ('map_user_stack', 'alloc_user_stack'))
    out = Path(tempfile.mkdtemp(prefix='welyos-stack-slot-concurrency-'))
    c = out / 'probe.c'
    c.write_text(FIXTURE.replace('CODE', code))
    exe = out / 'probe'
    subprocess.run(['cc', '-O1', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(exe)], check=True, capture_output=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    model = ownership_model()
    negatives = []
    for mutation in ('leave-pages', 'release-other-owner'):
        try:
            ownership_model(mutation)
        except AssertionError:
            negatives.append(dict(mutation=mutation, caught=True))
        else:
            raise AssertionError('uncaught model mutation: ' + mutation)
    changed = [p for p, s in inputs.items() if (ROOT / p).read_text() != s]
    result = dict(input_sha256={p: hashlib.sha256(s.encode()).hexdigest()
                               for p, s in inputs.items()}, changed_inputs=changed,
                  production_counterexample=dict(exit=run.returncode,
                                                 stdout=run.stdout, stderr=run.stderr),
                  acceptance_model=model, model_negative_controls=negatives)
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(run.stdout.strip())
    print('PASS ownership specification:', model)
    print('PASS model negative controls:', negatives)
    print('artifacts:', out, 'changed_inputs:', changed)
    return run.returncode or int(bool(changed))


if __name__ == '__main__':
    raise SystemExit(main())
