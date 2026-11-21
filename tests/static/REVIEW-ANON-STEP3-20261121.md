# C3.2 anonymous tickets: independent checks

## Passing results

`probe_anon_pt_bound.py` extracts unchanged production tables_at/pt_need.
148,400 counting cases passed with UBSan: boundary-aligned and adjacent pages,
both table levels, variable ranges and user bases 1GB/4GB. Independent table-set
counting gives M=249 for the configured contiguous heap+mmap union. Two mutations
(end rather than end-1; omitted upper table) were caught.

10,542,082 finite arithmetic states passed for M=1/2/3/7/249. Given outstanding
distinct demand D<=T and new distinct demand k<=pt_need, both bounded by M-E,
T+min(pt_need,M-E-T) covers total demand and preserves E+T<=M. A legacy table
creation at the cap can retire one redundant ticket without losing coverage.
This is a conditional algebra check, not proof that production E/T/D bookkeeping
always satisfies the premises. Production vm_set_anon_windows and
anon_table_built were reviewed, not executed by this counting probe.
Evidence: `/tmp/welyos-anon-pt-bound-otaknc9s/results.json`, unchanged input.

`model_anon_r1.py` exhausts a specification model with one virtual page and three
spaces: 76 canonical states and 432 transitions, including commit, first touch,
COW copy/reuse, unmap, and repeated fork trees. Every state checks total tickets
against independently updated reservations and writable physical exclusivity.
Two mutations (no parent fork ticket; no return on sole-owner COW) are caught.
Failed-fork pending arithmetic is also checked. This is a specification oracle,
not execution of product VM, nor coverage of multi-page segmented release,
intermediate pending interleavings or hardware concurrency. Those remain open.

## New integration defect reproduced: failed fork changes parent

`probe_fork_parent_failure.py` executes the production in_anon/fork_table bodies
with a child walk that refuses allocation. The first writable parent ANON PTE
is changed to COW|TICKET and fa.marked becomes 1 before the walk returns NULL.
The caller then adds marked to parent data_tickets and only unreserves
pending-marked. Destroying the child cannot undo this parent change.

This violates the agreed failed-fork contract (parent PTE/tickets unchanged).
It can leave an extra reservation held after an unsuccessful fork until that
parent page is written or released. It does not establish data corruption or
an unreserved COW fault; the retained ticket is conservative but observably
reduces future reservation capacity. Restore the agreed sequence: finish all
fallible child construction before publishing parent COW flags/tickets, or
implement a complete rollback with PTE/TLB and parent reservation restoration.
Evidence: `/tmp/welyos-fork-parent-failure-40zaee7v/results.json`, unchanged input,
UBSan clean. This probe is a known-counterexample reproducer: exit 0 means the
defect reproduced, not that fork passed acceptance. Add integrated failure
injection for every child table allocation and compare parent before/after.

## QEMU evidence cross-check

All three ELF hashes still matched manifest-step3.txt when inspected, as did the
three recorded log hash prefixes. Each log contains anon ticket success and
emulator exit 0. regress-anon.txt ends with all expected results. Current
vm.h/proc.c/proc_mem.c/vm.c have advanced beyond this manifest; do not attribute
these older binaries' passing tests to the newer fork integration. This review
did not run QEMU. The manifest's source list covers changed files, not a complete
source/configuration snapshot, and logs use truncated hashes; preserve complete
per-stage evidence in subsequent milestones.

No product edits, commit or push. Remaining acceptance: production R1 lifetime
tests, multi-page release/COW interleavings, fork failure rollback, stable source
manifest and three-architecture QEMU validation of the new integration.
