# File/pipe object-budget lifetime review — 2026-11-21

Scope: Claude message 172's current uncommitted file/pipe accounting. No product edits, baseline edits, QEMU, commit, or push by this reviewer.

`python3 tests/static/probe_filepipe_lifetime.py` passed 1,536 cases plus a saturated-domain-reference rejection. Evidence: `/tmp/welyos-filepipe-lifetime-3s7y8211/results.json` and generated C/executables. Four input source/header hashes are recorded there; `changed_inputs=[]`.

The harness extracts unchanged production functions: `mb_domain_get/put`, `fits_locked`, `mb_quota_add/sub`, `sub_locked`, `mb_charge_object/release_object`, `mb_pages_for_bytes`, `iget/iput`, `vfs_file_new/dup/close`, and `pp/pipe_closed/free/release/vfs_pipe`. Production `file`, `pipe`, and `mb_domain` declarations are extracted too.

Coverage:

- Each of the three constructor allocations fails independently; pipe creation also meets quota boundaries at pipe, first-file, and second-file charging. Failure must leave no allocated objects, quota, references, or accounting errors beyond the creator's original domain reference.
- Kernel construction with NULL domain, charged construction, two allocator-size paddings, limits 0–11, zero–three extra references per endpoint, and both final-close orders.
- Creator drops its original reference before inherited endpoints close. A distinct closer domain stays entirely unchanged; all creator-domain/object allocations disappear on final close.
- A saturated domain reference count rejects construction without quota or reference changes.
- Three mutations (omit file refund, omit pipe refund, leak the reference on charge failure) each fail with designated exit 11. Unmodified source exits 0. UBSan is enabled with recovery disabled; no diagnostics.

No new product defect was found in these exercised paths. This does not close unrelated stack rollback, image/exec, temporary objects, ramfs, or complete fork fault-injection work.

Limits: locks and scheduling are deterministic no-op fixtures; allocation/size metadata, inode initialization, and wait queues are fixtures. Read/write operations are excluded. This is not an SMP race, real heap-exhaustion, physical-memory reservation, file-I/O, or scheduler test. Fixtures simplify inode/mutex layout, so rounded charges validate the extracted algorithm against fixture allocation sizes, not every architecture's actual structure sizes. The stored source hashes should be checked before applying this evidence to later edits.

QEMU acceptance-strength issue (also noted by root): `pipe_kobj_test` currently checks `st.used[MB_KOBJ] >= 3`, while the milestone reports 4 pages. That assertion can accept an undercount. Add an independently derived exact expected cost using actual allocation-size visibility, and exercise a limit exactly one page below that cost plus constructor failure unwind. This is a test blind spot, not evidence that the current product undercounts.
