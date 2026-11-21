# sys_fork late preparation failure breaks parent rollback — 2026-11-21

**Reproduced caller-level failure atomicity defect.** `sys_fork` commits `pmem_fork` before preparing the kernel thread. If `thread_prepare` subsequently returns NULL, it destroys the child address space and domain, but does not restore the parent's PTEs or release the parent's newly reserved COW tickets. This is separate from the now-fixed failure inside the first pass of `fork_table`.

Run: `python3 tests/static/probe_tempobj_fork_late_failure.py`.
Evidence: `/tmp/welyos-tempobj-fork-late-58y5y1dl/results.json`, generated C and binaries. Four source SHA-256 values are included; `changed_inputs=[]`. UBSan recovery is disabled and no diagnostics occurred. Exit 0 means the counterexample and controls reproduced, **not** product acceptance.

For each parent containing 1–4 resident writable anonymous pages, inject NULL from the subsequent `thread_prepare`:

| Observation | Before | After failed sys_fork |
| --- | --- | --- |
| Return | — | `-E_NOMEM` |
| Parent resident PTE | WRITE, no COW/TICKET | COW + TICKET, no WRITE |
| Parent data_tickets | 0 | number of pages |
| PMM outstanding reservations | 0 | number of pages |
| Remaining child objects | 0 | 0 |
| Published child / thread started | none | none |

The child cleanup is balanced; this defect is the parent's extra promise and changed mappings after a failed syscall. It can reduce remaining commitment capacity until those parent pages fault or are released. No data corruption is claimed.

The probe extracts unchanged production `sys_fork`, `fork_child_main`, `pmem_fork`, `pmem_release`, `proc_kobj_pages`, `pmem_domain`; VM `vm_space_create/destroy`, `free_table`, `in_anon`, `fork_table`, `vm_space_fork`; and the actual membudget domain/quota/bundle functions. Thus `pmem_fork` or the VM commit is not a handwritten model. Successful controls execute real `fork_child_main` and verify that `fork_start` and frame allocations are freed and child register return value is zero. Moving the injected rejection before the VM fork is a local negative control: parent PTEs, tickets, and reservations then remain exactly unchanged in all four cases. This is a hypothesis control, not a proposed complete patch.

Fixtures replace scheduler/thread preparation, heap and table allocation, PMM counters/reference primitives, locks/TLB/SMP, PTE codecs/walk, and routine VFS/signal calls. `pmem_domain_new` supplies a fresh limit-1000 domain. A small two-level/four-entry page-table geometry exercises the real recursion; intermediate-table charging in the walk fixture is simplified. This is not real PMM exhaustion, architecture MMU execution, concurrency, or a complete `sys_fork` integration test. Failure is injected at the real caller's thread-prepare boundary.

Fix direction: prepare every fallible resource before committing the parent's VM changes, or implement complete parent rollback on every later failure. Preparing the child thread before `pmem_fork` likely requires splitting child-domain preparation from VM duplication and abandoning that prepared thread on earlier failure; simply skipping the thread preparation is not a fix. Add caller-level post-VM thread failure injection checking parent PTEs, data/pt tickets, reservations, page refs, publication, and allocations against the pre-call snapshot.

Only new reviewer-owned tests/report files were added. No product/baseline changes, QEMU/GPU, commit or push.
