# 隔離提案編譯檢查

`python3 tests/static/probe_proposals_build_current.py` 複製 kernel/include/arch
到 `/tmp`，僅在副本套用 `tempobj-kobj-20261121.patch` 與
`ramfs-zero-write-eof-20261121.patch`，未修改共享產品。

沿用各架構現有 CMake 產生的 compiler flags，額外 `-Werror`，i386、
riscv64、aarch64 的 proc.c 與 ramfs.c 六個 translation unit 均編譯通過，
無診斷，原始產品檔案前後 hash 不變。

證據 `/tmp/welyos-proposal-build-0h4qh1a2/results.json` 記錄完整編譯命令、
來源與 patch hashes，並保留套用後來源及 object files。

這是 compile-only，沒有連結 kernel 或 QEMU 驗收。ustart 提案的 host
生命週期驗證另見 REVIEW-TEMPOBJ-KOBJ-PROPOSAL；整合後必須調整既有
stack 故障測試，避免 ustart charge 提早失敗導致沒有真正測到 stack 回退。
RAMFS EOF patch 也仍是未套用提案，產品中的反例尚未修正。
