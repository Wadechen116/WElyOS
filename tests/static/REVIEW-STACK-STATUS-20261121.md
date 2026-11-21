# Stack rollback and exact thread charge: resumed independent review

The current product has not corrected the user-stack failures from message 191.
The existing probes were rerun rather than replaced with duplicate tools.
No applicable AGENTS.md was found in the repository or its workspace ancestors.

`python3 tests/static/probe_user_thread_stack_failure.py` reproduced both known
failures in the extracted production helpers: failure on initial page three
retains the first two mappings; fifteen subsequent kernel-thread preparation
failures retain sixty mapped pages and consume all fifteen remaining automatic
stack slots. The supplied-stack control remains unchanged. Evidence is
`/tmp/welyos-user-stack-failure-ovsqzqir/results.json`.

`python3 tests/static/probe_stack_slot_concurrency.py` also reproduced two
successful creators selecting the same stack top `0x6ff00000`, with four duplicate
virtual-page mappings while `nstacks` becomes 3. The independent ownership model
still covers 49 states and 160 transitions and catches both negative controls.
Evidence is `/tmp/welyos-stack-slot-concurrency-yqs0mli3/results.json`.

Both commands exit zero because the product counterexamples reproduced; this
is not product acceptance. Both use UBSan, emitted no sanitizer diagnostics, and
reported unchanged product inputs. Interleaving is injected at a helper boundary;
these are not actual SMP, PMM-accounting or QEMU results. The separate model does
not verify a production correction.

The precise KOBJ test assertion requested in message 190 is also still missing.
`kernel/test/mem_test.c:934` retains `k >= 5 && st.refs == 2`, and its limit failure
uses `k - 1`, where `k` comes from the product's own charge. Production sched.c
currently charges `STACK_PAGES + 1 + ceil(kmalloc_size(t)/PAGE_SIZE)` with
`STACK_PAGES=4`. Thus the product expression includes the guard page, but the
guest assertion still cannot independently reject omission of that page. This
is a test-strength gap, not a claim that the present production charge is wrong.
The guest check should independently derive five stack/guard pages plus rounded
allocation size and compare equality; boundary limits should use that expected
value. No new probe was needed to establish this unchanged source assertion.

Inspected hashes:

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 58e8d130dd29730fd9914696068806ca8ab560d1ca16a8c65c9f3947ca69bb86 |
| kernel/proc_internal.h | 58a278af41f0217acf4d488efaa056676b0de744b1f4aa5d581c4e042f337271 |
| kernel/test/mem_test.c | 8717bb8721753d900a00caa9baf5182d8b3c34c3f4e73145bc719b5afe586a17 |
| kernel/sched.c | 7748c32b0ed821bde02985abe3cbb4eda57d4611b7f8e00baf3c3dfb74a0f3c8 |

Owner-side corrections and integration acceptance remain required as described
in REVIEW-USER-STACK-FAILURE-20261121.md and
REVIEW-STACK-SLOT-CONCURRENCY-20261121.md. This review added only this report;
no product, baseline or existing probe changes, no QEMU/GPU, and no commit/push.
