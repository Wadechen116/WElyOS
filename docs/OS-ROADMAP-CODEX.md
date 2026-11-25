# OS 功能實作路線圖（Codex 版）

**狀態：** Codex 定版，供使用者決定是否開始實作。本文不代表任何階段已獲開工核准。  
**範圍：** WelyOS 一般 OS 與高可靠／航太方向的功能缺口。PowerShell QEMU 啟動器不屬於本路線圖。  
**文件所有權：** Codex 維護本文件；Claude 維護自己的對照文件。需要改對方文件時，先以訊息提出建議，不直接改寫。

**Claude 對照稿：** [docs/OS-ROADMAP-CLAUDE.md](OS-ROADMAP-CLAUDE.md)（Claude 擁有；待建立與互審）。

## 共作方式

- Claude 負責 kernel、架構與使用者層整合；Codex 負責可純 C host 驗證的核心、模型、測試向量／fixture 與獨立設計審查。
- 每個功能開工前，雙方先凍結架構、介面、狀態機、錯誤語義及正反測試；使用者決定是否開始。
- Host 測試通過不代表 QEMU 或目標板驗證通過。硬體能力、WCET、ECC、watchdog 與實際 jitter 仍須在對應平台驗證。
- 下表是依賴順序；可平行的純 host 工作可提前，但 kernel 整合須等前置契約通過。

## P0–P8 順序

| 階段 | 工作範圍與次序 | 前置依賴與完成門檻 | 分工 |
|---|---|---|---|
| **P0：I-349 收尾** | 修正診斷分類的 scope gating；使用 target 同 CPU 的低優先權 helper 做有效 spike 注入；保留原始觀測及理由碼。 | 診斷只分類、不豁免；原有 panic/assert/通過條件完全不變。完成 host 向量、三架構 none/steady 與有效 spike 驗證。 | Claude：kernel 測試收集與注入。Codex：純分類器及 host 測試。共同凍結分類門檻與測試向量。 |
| **P1：需求與基礎介面** | 先做 I-333 目標硬體／介面／負載需求模板；將窄版 P5a 裝置註冊、IRQ、完成通知與初始化失敗語義提前。再依序做 monotonic/POSIX 時間、FD readiness/poll/nonblocking、訊號與 session/job control、關機／重啟與 A2 watchdog、符號連結。 | 時鐘先於 poll timeout、alarm 與 I-332 量測；FD readiness 先於通用 socket；裝置介面先於中斷驅動。未指定目標板前，只做 I-333 模板及可移植抽象，不宣稱支援特定飛控／相機。 | Claude：kernel、arch、VFS 與平台整合。Codex：readiness/timeout/cancel/close 模型、純規則測試及 API 審查。I-333 硬體需求由使用者補充後再定平台工作。 |
| **P2：儲存** | 定義 block flush/barrier 與錯誤契約；WFS journal 格式、checksum、replay/recovery；MBR/GPT parser；block IRQ 完成；最後把根目錄移到磁碟並定義啟動回退。Journal 核心與分割區 parser 的 host 工作可平行。 | Journal kernel 整合依賴可驗證的寫入順序／flush 語義；root-on-disk 依賴 journal recovery 與唯讀降級策略。分割區不是第一版 root 的硬依賴，可先用 whole disk。Polling I/O 可先建立正確性，不必為了效能阻塞 journal 核心。 | Claude：block/VFS/WFS/啟動整合。Codex：journal 格式與 replay 純 C 核心、crash-point fixtures、MBR/GPT bounds/overflow/CRC host 測試。 |
| **P3：記憶體功能** | mprotect → 明確 shm API → page cache 與檔案 mmap → swap／磁碟 demand paging（最後）。 | 檔案 mmap 依賴 page cache 及檔案截短／刪除語義。Swap 開工前重新證明 commit 必兌現、配額、fork/COW、回收、I/O 失敗與 writeback error 的不變量。 | Claude：VM、page cache、syscall 與錯誤回復。Codex：配額／admission/accounting 模型與不變量 host 測試；模型通過不等於 VM 正確性證明。 |
| **P4：網路** | 通用 socket（依賴 P1 readiness）→ loopback 與 IPv4/UDP 能力 → TCP 與 DHCP → 網卡 IRQ／多 NIC → IPv6。把 loopback、分片重組、broadcast/multicast 拆成可驗收項；TCP 與 DHCP 的純協定核心可平行。 | DHCP 依 UDP，不依賴 TCP。網卡 IRQ 依賴 P1 裝置介面；PCI NIC／多 NIC 還依賴 P5 PCI/resource 工作。IPv6 需鄰居探索、路由與 socket 契約成熟後再做。 | Claude：kernel network、socket 與 driver 整合。Codex：IPv4 parser/reassembly bounds、TCP state/retransmit、DHCP encode/decode 純 C host 核心與測試向量。 |
| **P5：裝置探索與 I/O** | P5a 已提前至 P1；本階段做 PCI/resource/IRQ 探索、PS/2 鍵盤、framebuffer，最後做 USB/XHCI/HID/storage。 | PCI 資源及 IRQ routing 成熟後，才能接 PCI NIC 與通用多裝置；USB 是大型獨立里程碑。P4 的 virtio-mmio／NE2000 協定工作可先行，但 PCI 裝置擴充等 P5。 | Claude：各架構列舉與驅動。Codex：可抽離的 PCI/分割區資料格式、邊界與錯誤案例 host 測試；不以 host 測試取代硬體驗證。 |
| **P6：SMP 效能與高可靠性** | 先量測 I-332 的資源上限、延遲／抖動與 workload；依 profile 再縮小 sched_lk、調整喚醒親和及 mutex 策略。A3 RAM scrub/EDAC 模型在記憶體契約穩定後做；A5 時間／空間分區最後。 | P6 效能修改須有前後可比的 workload 與回歸。A4 依賴 P1 clock；A5 依賴 scheduler、記憶體、IRQ 與裝置隔離。目標板未知時，WCET/ECC/硬體 watchdog 只能列驗證計畫，不能宣稱已證明。 | Claude：排程器、平台 watchdog、實機量測與整合。Codex：admission/partition 算術模型、測量資料分析與 fault-injection host 模型。 |
| **P7：使用者層與 ABI** | syscall/errno/time/signal/FD ABI 穩定後補 POSIX libc、基本工具與編輯器；dynamic linker 放在 file mmap/page cache 穩定之後。 | 基本 libc/工具可在 P1–P3 介面穩定後逐步開始；dynamic linker 不得早於 P3 檔案映射契約。這一階段是整合完成門檻，不要求所有使用者工具都等到此時才動工。 | Claude：user ABI、libc 與工具。Codex：可 host 測試的 parser／格式／相容性向量及介面審查。 |
| **P8：新架構與開機方式** | x86_64 與 UEFI 分開里程碑：先完成新架構 ABI、MMU、例外／中斷、SMP 與必要 driver；再按目標架構接 UEFI boot protocol。 | P1–P7 的穩定介面可減少移植分支；x86_64 與 UEFI 各自有獨立 boot、回歸與除錯驗收，不能以單一「可開機」標記視為完成。 | Claude：架構與 bootloader 整合。Codex：host 可測的 ABI/解析器/格式核心及跨架構契約審查。 |

## 順序可能造成的返工

1. 若 P5a 不提前，P2 block IRQ、P4 NIC IRQ、PCI 與 USB 會各自形成不同註冊／完成模型，之後再統一成本高。
2. 若 P1 monotonic clock 晚於 poll、alarm 或 I-332，timeout 與 latency 定義會分裂。
3. 若 journal 未先定 flush/barrier 與 crash recovery，就先做 root-on-disk，斷電後的啟動與唯讀降級很難保證。
4. 若 file mmap/page cache 未穩就做 swap，commit accounting、回寫與 fork/COW 會互相牽動。
5. 若通用 socket/readiness 未定就做 TCP，協定狀態機會綁死在 UDP 專用 FD 上。
6. 若未量測就先拆 sched_lk，會同時增加競態面與除錯成本，且無法證明效能改善。
7. I-333 的目標板未定是硬體支援項目的需求 blocker，不阻礙先做模板、host 模型與 QEMU 可驗證的抽象。

## 開工規則

本路線圖只定順序與分工，不授權開始任何階段。每個功能開始前，Claude 與 Codex 仍須按四節設計審查（架構／狀態與並行、設計模式、低耦合介面、高彈性），列出含負對照的測試、風險與 host/target 證據範圍；由使用者決定是否開工。
