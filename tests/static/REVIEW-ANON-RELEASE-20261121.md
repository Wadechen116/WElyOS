# Segmented anonymous release: independent host check

`python3 tests/static/probe_anon_release_segments.py` executes the unchanged
production vm_space_release_anon body with a fixed PTE array, lock assertions,
quota/reservation fixtures, and deterministic COW transitions at unlocks.
18,496 schedules pass: lengths 1/63/64/65/127/128/130, eight state rotations,
every target page, first/second unlock, and copy versus sole-owner reuse.
Mappings mix absent, exclusive, and ticketed COW states. Assertions check all
remaining mappings, data tickets, ANON quota and reservation are zero, the
expected release segment count, and exact reference-release/TLB callback counts.

The unlock callback models another actor resolving a ticketed COW on a future
segment: remove its ticket, change PTE to exclusive, and optionally release its
old shared reference. This forces the production release loop to observe the
updated PTE when it reacquires vm_lk. It does not run production vm_space_fault,
actual threads, actual MMU/TLB shootdown or real PMM/membudget. Therefore this
checks deterministic release interleavings at the helper boundary, not full
concurrent VM integration. Multiple pages undergoing COW per unlock, cross-space
reference races, fork and scheduler behavior remain outside the fixture.

Two mutations (omit ticketed-COW return; omit data ledger decrement) both fail
the exact end-state assertion with exit 10. UBSan clean; source unchanged during
execution. Evidence: `/tmp/welyos-anon-release-kwfnf9s_/results.json`, including
source SHA-256. No product source edits, QEMU run, commit or push. This is an
additional acceptance component and does not close remaining R1 integration gaps.
