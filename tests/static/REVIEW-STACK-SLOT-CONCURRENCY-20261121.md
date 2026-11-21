# Stack slot ownership and rollback: independent check

## Current product defect reproduced

No stack rollback fix was present at inspection. `alloc_user_stack` still reads
`p->nstacks`, maps the selected stack, then increments `nstacks` without a lock or
per-slot claim. `map_user_stack` and `user_thread_prepare` also retain the failure
paths documented in REVIEW-USER-STACK-FAILURE-20261121.md.

`python3 tests/static/probe_stack_slot_concurrency.py` executes the unchanged
production `map_user_stack` and `alloc_user_stack` bodies. A scheduling callback
pauses the outer creator after selecting slot 1 but before its first page map;
a second creator then completes its allocation before the outer resumes.
Both calls return the same nonzero stack top `0x6ff00000`, all four page addresses
are mapped twice, and `nstacks` ends at 3. Thus two successful preparations can
share a stack even though the counter claims two different slots were allocated.

The callback models a permitted interleaving at the helper boundary; it does not
run OS threads or the real page mapper. The fixture records duplicate virtual
addresses. In production vm_space_map permits replacing an existing mapping, so
overlapping stack ownership is a material correctness issue; this probe does not
claim to quantify physical-page loss or demonstrate a real guest crash.

Evidence: `/tmp/welyos-stack-slot-concurrency-tfou7hx2/results.json`.
UBSan emitted no diagnostic; `changed_inputs=[]`.

- kernel/proc.c SHA-256:
  `58e8d130dd29730fd9914696068806ca8ab560d1ca16a8c65c9f3947ca69bb86`
- kernel/proc_internal.h SHA-256:
  `58a278af41f0217acf4d488efaa056676b0de744b1f4aa5d581c4e042f337271`

## Concrete ownership acceptance model

The same tool exhausts a separate specification model with two creators, two
reusable slots and two initial mapped pages per slot: 49 states, 160 transitions.
Each slot progresses free -> owned/preparing -> live. Claim is atomic; mapping
increments only the owner's page count. Failure removes that owner's pages and
claim; publication requires all pages present. Release of another creator's
resources is forbidden. Repeated reuse after failure/exit reaches a fixed point.

Negative controls retaining pages on rollback and releasing another owner's slot
are both caught. This model is an executable acceptance contract, not product
code verification or a production patch. The reduced two-page model checks
ownership/state transitions; the product counterexample executes its actual
four-page mapper.

A high-water counter with blind `nstacks--` is not a safe correction: A claims
slot 1, B claims slot 2, A fails and decrements 3 to 2, then C reuses B's live
slot 2. A future fix needs explicit ownership (for example a bitmap under the
process memory mutex) or equivalent serialization that covers claim through
rollback. A prepared request must retain its exact slot identity until start or
abandon. Rollback must remove only pages installed by that request, including
all partially installed pages. Supplied user stacks must bypass automatic slot
claim/release. Coordinate with fork/exec snapshots and stack reservation tickets.

## Required integration evidence

After owner-side fixes, execute production helpers with failures at each page,
post-stack thread preparation failure, immediate successful retry, two creators
interleaved through claim/map/publish/rollback, and one creator failing while a
different creator is live. Verify slot uniqueness, old PTE/content preservation,
per-request page ownership, quota/ticket/reference balance and absence of PID/TID
publication on failure. Then run three-architecture QEMU with stable binary
manifests; actual SMP creation and pressure remain necessary.

No product files, baseline or existing tools were changed. No QEMU/GPU,
commit/push, or direct memory-server write was performed by this sub-agent.
Neither stack rollback nor concurrent slot allocation is closed by this work.
