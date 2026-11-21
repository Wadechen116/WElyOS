# Failed user-thread preparation retains stack resources

Production map_user_stack, alloc_user_stack and user_thread_prepare were
executed with failure-injecting map_page/thread_prepare dependencies using
`python3 tests/static/probe_user_thread_stack_failure.py`.

Two counterexamples reproduce:

1. The third of four initial stack-page allocations fails. The first two
   mappings remain, while nstacks has not advanced and the caller reports failure.
2. All four stack pages map successfully, then kernel thread_prepare fails.
   Only ustart is freed. nstacks and four mappings remain. Starting with one
   existing stack, fifteen such failures consume the remaining fifteen slots
   and retain sixty mapped pages. The next automatic-stack request is refused
   despite no additional thread having started.

The fixture uses current PROC_MAX_THREADS=16 and USER_STACK_PAGES=4; dependencies
model mappings, allocation counters and kernel preparation failure. The three
functions controlling rollback are unchanged product bodies. A supplied-stack
control confirms it creates no automatic stack or slot. This is helper failure
evidence, not a real PMM accounting or QEMU demonstration. Exit 0 means the known
counterexamples reproduced, not acceptance passed. Evidence:
`/tmp/welyos-user-stack-failure-e6zreiwf/results.json`, unchanged input, UBSan clean.

Required correction: make automatic stack creation transactional. Partial
mapping failure must unmap each newly created page. Later kernel-thread failure
must relinquish this request's stack pages and slot without touching caller-
supplied stacks. Coordinate slot reservation/rollback with concurrent thread
creation; blindly decrementing nstacks is unsafe when another creator may have
claimed a later slot. Existing intermediate page tables may remain charged if
the VM intentionally retains them, but mapping and slot leaks must be removed.
Re-evaluate when stack reservations are integrated so tickets/quota follow the
same rollback ownership. Add failure at each page and post-stack preparation,
then successful retry plus concurrent creators to host/QEMU coverage.

No product edits or commit/push; communicated to Claude for owner-side fixes.
