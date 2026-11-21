# vm_space_fork caller rollback: independent host probe

`python3 tests/static/probe_fork_caller_rollback.py` passes 640 schedules using
17 unchanged product function bodies from kernel/vm.c, including
vm_space_create, vm_space_fork, vm_space_destroy, fork_table, walk, new_table,
anon_table_built and free_table. This adds caller unwind coverage beyond the
existing 6480 fork_table-only mixed-leaf schedules.

The fixture uses two- and three-level page tables with a reduced two-bit radix.
It populates every third page in the lower half of the synthetic user range,
mixing writable leaves and pre-existing COW/TICKET leaves, while the upper half
remains committed but untouched. Initial parent page-table tickets are tested
both at zero and at M-E; the latter satisfies the existing E+T<=M invariant.
Actual production table walking, table allocation accounting, recursive freeing,
child ledger setup and parent pending-ticket rollback execute in the probe.

Failure injection covers every allocation position encountered by these trees:

| Dependency | Two-level failures | Three-level failures |
| --- | ---: | ---: |
| vm_space structure allocation | 2 | 2 |
| PMM allocation: root and each child table | 6 | 22 |
| child domain reference acquisition | 2 | 2 |
| root and child-table quota acquisition | 6 | 22 |
| child bundle or parent pending reservation | 4 | 4 |

These are 72 injected failures total; 568 additional schedules complete a fork
and destroy the child. The latter include injection indices beyond the actual
dependency call count, so the 640 total must not be presented as 640 distinct
failure sites. The reduced two-level tree needs three child allocations, and
the three-level tree needs eleven; indices 0..31 cover all of them in each of
the two parent-ticket configurations.

For every failed fork, the harness checks that the parent vm_space structure,
all existing page-table bytes, initialized data-page bytes, parent and child
domain quota/reference counters, PMM reference counts, reserved count, table
count and vm_space heap count match their pre-call values. Every newly allocated
child physical block must have zero references after unwind. Successful forks
are checked for the expected additional parent tickets; child destruction must
restore child-domain and data-page references. Parent destruction then returns
all tracked resources to zero, leaving only each caller-owned domain reference.

Three negative controls are caught in both geometries: omit parent pending
unreserve, omit failed-child destruction, and omit root quota rollback. Each
returns the harness's designated accounting-failure exit 90. Baseline runs exit
zero, emit no UBSan diagnostics, and report unchanged product input.

Evidence: `/tmp/welyos-fork-caller-2f0kag3k/results.json`, including the source
SHA-256 and all baseline/mutation results plus generated C/executables nearby.

This is host verification of extracted production VM control flow with fixture
PMM/quota/reference operations, synthetic PTE encoding and synthetic geometry.
It does not execute the real PMM or membudget implementations, architecture PTE
formats, TLB/shootdown behavior, SMP or guest QEMU. Locks and shootdowns are
instrumented stubs. Quota failures are injected directly rather than obtained
through real pressure; integer overflow and all range/flag combinations are not
covered. It does not check successful vm_space_fork followed by later pmem_fork
or sys_fork failures; that enclosing transaction still requires its own review.
No claim is made that the entire fork transaction or memory-budget project is
accepted. No new VM-internal counterexample was found in this bounded coverage.

Only this report and its own probe were added; product, baseline and existing
probes remain untouched. No QEMU/GPU, commit or push was performed.
