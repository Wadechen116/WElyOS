# Codex 複核 109：故障注入與分析更新（2026-11-18）

本輪處理 A1、A2、B2、C。產品程式交 Claude 修正；未執行 QEMU、未寫 build/、未提交或推送。
B1 排程器、B3 黑盒子、B4 probe 及 D 使用者程式提示分類仍待後續審查。

## TMR 故障矩陣：26 案例，21 通過、5 重現缺陷

執行 `python3 tests/static/probe_tmr_matrix.py`。原始 `user/tmr.h` 複製到 /tmp，
僅替換 libc/syscall 外界環境；使用 Linux/POSIX 行程、管線及 WElyOS 的 E_* 常數。
以 ASan/UBSan、`-Wall -Wextra -Werror` 編譯。結果：
`/tmp/welyos-tmr-matrix-8pmqdj31/results.json`。

涵蓋 gate/out pipe 各次失敗、三次 malloc 失敗、四次 fork 失敗、dup2、子行程 gate read、
父行程 read/write/short write、子行程 write、四次 waitpid 失敗，以及超量輸出、
提早關閉 stdout 後停滯、timer 在持續輸出時到期。

- 107 原有的兩個 fork 卡住問題已修好。全部案例沒有 fd 數量增長，沒有 buffer 所有權異常，
  沒有外部超時；ASan/UBSan 無診斷。所有 res 槽位皆為 NULL 或本輪成功配置的指標，
  harness 會依契約逐一 free。這不代表原始所有呼叫者都會 free（vote 是結束行程回收）。
- **TMR-1：第一個 drain read 失敗可能被隱藏。** `tmr_run` 第一個 while 的 read 得到負值後，
  第二個 while 再讀到 EOF，覆寫 n=0，最後 `if (n < 0)` 沒有標記 invalid。
  parent-read#1 注入 -E_IO、各份均空輸出時，實際 rc=0、agree=3、invalid=[0,0,0]。
  應立即保留第一次讀取失敗，不讓後續讀取覆蓋。
- **TMR-2：waitpid 一次失敗就放棄回收。** waitpid#1..3 注入 -E_INTR，各留下 1 個未回收副本；
  #4 留下未回收的 timer（在檢查當下仍可能尚未完成終止）。
  應明確區分可恢復的中斷與永久錯誤，做有界清理並確認回收。
  harness 在 tmr_run 返回後立即用 WNOHANG 檢查，再主動清理，不把自己清理的結果算作產品通過。

兩項均為主機模型確認；內核對 EINTR 的訊號時序、管線排程與 POSIX 不完全相同，仍需系統整合驗證。
探針發現上述缺陷時 exit 1，並非所有測試都已通過。

## Allocator：主機 64 位元、單執行緒模型通過

執行 `python3 tests/static/probe_allocator.py`。保留原始 libc.c 實作、全函式名稱隔離，
以固定 4 MiB arena 模擬 sbrk，未配置及已縮回區域以 ASan poison。
鎖替換為單執行緒 no-op；libc.h 複本僅將 format attribute 改用 GCC 的保留拼字，
避免名稱隔離巨集改寫 attribute。原始產品檔案不變。

驗證初次 sbrk 失敗後可恢復、realloc 成長失敗保留原內容、縮回失敗不損壞 heap、
恢復縮回、SIZE_MAX 附近與超出 1 GiB 上限的大小、calloc 乘法邊界、零大小、
1024 次保留內容的 realloc。結果 returncode 0，ASan/UBSan 無診斷，無編譯警告：
`/tmp/welyos-allocator-2z501zgd/results.json`。

不涵蓋 32 位元機器、真實多執行緒互斥、惡意無效 free 指標或真實核心記憶體配額。

## SEU：請求的旗標與參數沒有一起取出

`kernel/proc.c` 的 `user_return_check` 在 exchange 清除 seu_pending 後才讀 seu_addr/seu_mask；
`SYS_seu` 的 writer 雖持 sched_lock，reader 不持鎖，另一 CPU 可以插入第二筆請求。

執行 `python3 tests/static/probe_seu_race.py`：摘取原始 SEU 消費區塊，
在 exchange 完成與 payload 讀取之間，確定地插入 writer B（等價於另一 CPU 在此完成 SYS_seu）。

1. A 請求：addrA、mask1、pending1。
2. reader exchange 清 pending 後，writer 發布 B：addrB、mask2、pending1。
3. 第一個 reader 卻消費 B payload，B 從 0 變 2；A 沒有執行。
4. 下一次 return 再消費 B，B 從 2 回到 0。

實際最終 [A,B]=[0,0]，預期 [1,2]，無 UBSan 訊息。
結果 `/tmp/welyos-seu-race-w664_1_u/results.json`，exit 1 表示已重現錯誤。
建議在 sched_lock 下把 pending、addr、mask 一起快照到區域變數並清 pending，
之後放鎖才存取使用者記憶體。若同一個行程有多條執行緒，同位址 XOR 並行與
「由哪條執行緒消費」仍需明確定義；只修 pending 的 atomic load 不足以修復此問題。
這是確定交錯模型，不是完整 kernel、TSan 或硬體記憶體模型證明。

## GCC analyzer 重跑與診斷分類

第一次掃描完整執行 210 TU（i386 72、riscv64 69、aarch64 69），58.495 秒。
一般診斷由 69 增至 74，too-complex 覆蓋缺口由 963 增至 1112。
掃描當下 changed_inputs/changed_tools 皆空；之後 Claude 開始搬移 kernel/main.c，
因此此份快照不能作搬移後來源的最新結論。原始完整 trace：`/tmp/welyos-codex-110-analyze`。

74 筆診斷可分為：

| 數量 | 類型 | 人工判讀與優先度 |
| --- | --- | --- |
| 33 | 未初始化值 | 其中 30 筆是 user syscall 透過 inline asm 輸出記憶體，GCC 無法推得內容；另 3 筆在 proc 的 sys_pipe/copy_to_user。vfs_pipe 成功時兩個輸出皆已賦值，現有 trace 未證明真缺陷。應追查模擬 syscall 輸出契約，不應先把所有 struct 清零來消音。 |
| 21 | va_arg 型別不符 | heap_corrupt 的固定 `%s` 被分析為可能走到其他格式分支，trace 未保留格式關係；未取得實證前不列為確診。 |
| 13 | NULL 存取／寫入 const | 已查看對應位置，屬於刻意的 crash 測試。移檔後應追蹤函式名稱與 hash。 |
| 5 | malloc leak | trace 到 tmr 子行程的 sys_exit inline asm，GCC 不知道行程退出會釋放位址空間；不能據此判定父行程洩漏。故障矩陣另行核對父行程 buffer 所有權。 |
| 2 | ptrdiff | rootfs_end-rootfs_start 的 linker symbol 邊界；屬於 freestanding/linker 契約的可攜性事項，未證明目前連結映像錯誤。 |

目前這 74 筆中沒有新增已重現的產品缺陷；值得優先修的是上方人工故障注入找到的 TMR 與 SEU。
三個 target 的 analyzer 仍為 partial，cppcheck 未安裝，完整分析 exit 2，不能宣稱無缺陷。
保存到專案的 findings-analyze.json 會移除冗長 diagnostic trace，保留訊息、來源 hash、位置與覆蓋狀態；
完整 trace 保留 /tmp artifact。

## 搬移函式的 baseline

提示 identity 包含 rule、檔案路徑、函式名稱、函式 token hash 與 detail。
因此函式內容完全不變但移到另一個檔案，ID 仍會變。

### 搬移後的最終掃描與 baseline 核對

- 重新掃描 HEAD a085f47 + step43 工作目錄，147 C/H、1117 函式、222 TU
  （i386 76、riscv64 73、aarch64 73）全部完成，68.509 秒。
  changed_inputs/changed_tools 皆空。仍有 74 筆一般診斷、1047 筆 too-complex（原先963），
  分類與上表相同。三架構 partial、cppcheck unavailable，因此 exit 2。
  最新報告與快照已寫入 findings-analyze.json、snapshot-analyze.json。
  完整 trace：`/tmp/welyos-codex-110-analyze-post-split`。
- 搬移後 13 筆 baseline 皆核對：舊精確 ID、新精確 ID、rule/symbol、相同函式 token hash；
  fs_list 的深度限制、overflow_recurse 的刻意故障範圍，以及 disk_demo/disk_stage_test
  的清理與後續 assertions 前提維持，因此補上目前的 cross_checked_by=Codex。
  不是用檔案搬移當作批量接受新提示的理由。
- fast delta：38 個 newly-unreviewed、860 個 existing-unreviewed、51 個 resolved，exit 1。
  38 個新增提示全部是 missing-check/function-length，0 個新 hard rule 提示。
  函式搬移會改變提示 ID，所以 delta 數量變動不等於多出38個產品bug。
  總未審查仍898，尚未重跑／刷新 fast reference，由 Claude 整合。
- 工具單元測試21/21通過，formatter star/digits/bounds 三模式 returncode0、無UBSan訊息。
