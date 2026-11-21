# Thread KOBJ lifecycle host review

`probe_thread_budget.py` executes unchanged production thread_prepare,
thread_uncharge, thread_abandon and thread_free extracted from sched.c.
3,840 finite cases passed with UBSan: five allocation sizes around page-rounding
boundaries, four existing quota usages, sixteen limits (including limit<used),
thread allocation failure, domain-get failure, absent/present domain, and abandon
versus deferred-free cleanup. Invalid priorities are checked to reject before
allocation. Successful prepare must charge exactly STACK_PAGES+1 plus the rounded
thread allocation and retain one domain reference. Cleanup restores original
quota/reference counts, frees stack/structure once and handles zombie count.

Three mutations are detected: missing domain put on cleanup, omitting the guard
page charge, and failing to put the reference after quota refusal.
Evidence: `/tmp/welyos-thread-budget-ud5hehvh/results.json`, source SHA-256 included,
`changed_input=false`; no UBSan diagnostics.

Fixtures substitute thread_alloc, allocator sizes, MB operations and stack/pool
returns. This checks caller lifecycle bookkeeping, not the real allocator,
stack guard restoration under exhaustion, actual scheduler, thread_start,
multi-core deferred reclamation, process start/fork publication, file/pipe KOBJ,
or use-after-free behavior. Real PMM/MB were tested separately; this tool does not
combine them. Stack/image reservation gaps remain open. b2 source was under
development; target QEMU integration still requires a stable manifest and actual
new-feature acceptance tests. No product edits, QEMU run, commit or push here.
