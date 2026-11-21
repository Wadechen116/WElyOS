# Stack bitmap correction: rollback progress and new preparing-slot race

The in-progress bitmap correction resolves the previously reproduced sequential
partial-map and post-stack prepare-failure examples. It also gives two concurrent
creators distinct slots. It does not yet isolate a preparing slot from another
existing thread's stack-growth fault.

`python3 tests/static/probe_stackfix_transaction.py` extracts the unchanged actual
`map_page`, `map_user_stack`, `unmap_user_stack`, `alloc_user_stack`,
`free_user_stack_slot`, `user_thread_prepare`, and `proc_fault_locked` functions.
The fixture executes 14 baseline checks (including four repeated prepare-failure
checks): failure at each of four PMM and map positions, kernel preparation
failure, successful retry, supplied-stack preservation and nested second creator
uniqueness. These baseline checks pass. Removing partial rollback or slot release
is caught at designated exit 10. UBSan has no diagnostics; product inputs remain
unchanged during the run.

## Blocking counterexamples

`alloc_user_stack` publishes the stack_slots bit before mapping/preparing the
stack. At this point it holds neither mem.mx nor pstate_lk during mappings.
`proc_fault_locked` accepts that bit as permission to grow the stack. A different
already-running thread of the same process can fault this slot during preparation.
The test injects this interleaving in the VM-map callback, after claim and before
the first initial mapping is installed, executing the real fault helper.

1. Fault eight pages below the new stack top; growth succeeds. Then the intended
   thread_prepare fails. Rollback removes only the four initial pages and clears
   the bit. One deeper mapping and its physical page remain with no claimed slot.
2. Fault at the first initial stack page; growth succeeds. The creator's
   map_page subsequently calls vm_space_map on the same address. Production
   vm_space_map replaces the existing PTE without releasing its previous PA.
   The fixture mirrors that documented implementation. Rollback removes the
   new mapping, but the earlier fault's physical page is leaked. The observed
   state has zero mappings and one retained PA.

Therefore the first baseline PASS line is not acceptance of the transaction.
Exit zero means both new counterexamples reproduced as expected. Fault rejection
for an unclaimed slot is already handled by the current code; the missing state
is specifically a slot that is claimed but not yet committed/live.

Consider separate preparing/live visibility with explicit promotion on success,
combined with sequencing fault validation/mapping and failure cleanup; or hold
mem.mx across preparation and rollback with a consistent lock order. Merely
switching the initial map to map_new prevents replacement but does not remove a
fault-created deep page on later failure. Merely testing the bit twice is not an
atomic transaction. Any correction must preserve independent creator ownership
and caller-supplied-stack behavior. Fork/exec snapshot ordering also needs review.

## Evidence and limits

Evidence: `/tmp/welyos-stackfix-transaction-v3mast07/results.json` and generated
C/executables. Source hashes at extraction:

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 414b3bb69c95bcac389d6d8733f652c3a2eb5e54611afe5e56fbd556bd17c247 |
| kernel/proc_mem.c | dfb34cc831db0ed0afdad308a487a8d08ab8c7d3a238550244ae73001249074e |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |

PMM, VM maps, allocation, scheduler and lock implementations are fixtures.
The no-op scheduling injects a permitted cross-thread interleaving; this is not
actual SMP or a QEMU crash demonstration. The real fault wrapper takes mem.mx,
but preparation currently does not, so that wrapper does not exclude the
injected overlap. The VM replacement behavior is mirrored from production
rather than running the actual VM functions. All files remain owned by Claude;
this reviewer changed only this report and its new probe, no product, baseline,
QEMU, GPU, commit or push.
