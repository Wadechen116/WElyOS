# C3.2(b1) step 2: table-allocation policy verification

`python3 tests/static/probe_vm_walk_budget.py` passed 768 finite combinations
against the unchanged production `table_alloc` and `new_table` bodies extracted
from `kernel/vm.c`. Evidence: `/tmp/welyos-walk-budget-yosk7r_j/results.json`,
including the source SHA-256 and `changed_input=false`.

Cases combine three walk policies, present/absent domain, 0..3 reserved tickets,
0..3 domain used/limit values (including a lowered limit), and success/failure of
ordinary allocation. Assertions check quota and local ledger, reserved tickets,
ordinary versus reserved allocator selection, table count, and full-page zeroing.
ANON with no tickets, including NULL space, actually calls the panic fixture.
Legacy allocation leaves anonymous tickets untouched. Quota refusal does not call
the allocator; allocator failure rolls back the provisional domain charge. ANON
consumption does not charge PGTABLE again.

Three isolated negative controls were caught: legacy stealing a ticket, omitted
quota rollback after ordinary allocation failure, and ANON charging quota again.
UBSan emitted no diagnostic.

These results verify allocator policy, not an entire page-table walk. Dependency
fixtures model domain arithmetic and allocator outcomes; the real PMM and
membudget were independently tested earlier and are not executed by this probe.
Root construction/destruction, shared-domain concurrency, partial multi-level
walk failure, mapping lifecycle and ticket ownership still need integration
coverage. An unsuccessful multi-level walk can retain an intermediate table that
was successfully created earlier; that table must remain charged until destroy,
so whole-walk failure must not be advertised as leaving all state unchanged.

Claude's step-2 QEMU tests in message 163 are separate evidence; complete regression
results were still pending at the time of review. Before accepting the integration
milestone, record target-architecture boot and new-feature QEMU results, as the
user requested. Host results cannot replace this requirement. No product files
were edited by this review and no commit/push was performed.
