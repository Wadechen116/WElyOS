# OS 功能實作路線圖(Claude 對照文件)

**對照的定版文件:** `docs/OS-ROADMAP-CODEX.md`(Codex 維護,順序與分工以它為準)。本文件由 Claude 維護,涵蓋同一個 P0–P8 定版順序、依賴與返工風險、雙方分工與使用者開工門檻,並記錄 Claude 這一側的工作、介面交接點與進度;需要改 Codex 文件時以訊息提出,不直接改寫。PowerShell QEMU 啟動器不在本路線圖內(使用者說明已另外提供)。

**狀態:** 順序與分工定稿;**任何階段(包括 P0)都尚未獲使用者核准開工。**

## 共作規則(使用者決定,2026-11-24)

- 每一項開工前:Claude 與 Codex 各寫一份設計審查(架構 / 狀態與並行、設計模式、低耦合介面、高彈性,加測試計畫與風險)→ 互相提供 → 互給建議 → Codex 定版 → 使用者決定是否開工。
- 分工:Claude 負責 kernel、架構、VFS、驅動、使用者層整合與所有 QEMU 驗證;Codex 負責可純 C host 驗證的核心、模型、測試向量與獨立設計審查。
- 交接點是「凍結的介面」:純 C 核心以只依賴 `<stdint.h>` 的標頭提供,kernel 與 host 測試引用同一份,不各寫一份模型。
- 每一項都要有修正前失敗 / 修正後通過的證據與負對照;跑完整回歸或長時間 QEMU 前先問 Codex 是否需要機器,並更新共享資源狀態。

## Claude 側的工作(依定版順序)

| 階段 | Claude 負責 | 交接點(Codex 提供) | 狀態 |
|---|---|---|---|
| P0 I-349 收尾 | kernel 端收集(逐窗、受測 / 對照樣本、進度、注入紀錄)、CPU1 的有效 spike 注入、arch-check 規則、改名 TARGET-PATH-CANDIDATE、QEMU 驗證 | `include/i349_classify.h` 純分類器與 host 測試(介面凍結中,msg 250) | 介面凍結中 |
| P1 | I-333 需求模板;窄版裝置 / IRQ / 完成通知介面;monotonic 與 POSIX 時間;FD readiness / poll / 非阻塞;訊號與 session / job control;關機 / 重啟與硬體看門狗;符號連結 | readiness / timeout / cancel / close 模型與規則測試 | 未開始(等設計審查) |
| P2 | block flush / barrier 契約、WFS journal 的 kernel 整合、block IRQ、root-on-disk 與啟動回退 | journal 格式 / replay 核心、crash-point fixtures、MBR / GPT parser | 未開始 |
| P3 | mprotect、shm、page cache 與檔案 mmap、swap | 配額 / accounting 不變量模型 | 未開始 |
| P4 | 通用 socket、loopback、IPv4 能力、TCP / DHCP 的 kernel 整合、網卡 IRQ / 多 NIC、IPv6 | IPv4 reassembly、TCP 狀態機 / 重傳、DHCP 純 C 核心 | 未開始 |
| P5 | PCI / resource / IRQ 列舉、PS/2、framebuffer、USB | PCI / 分割區格式的 host 測試 | 未開始 |
| P6 | I-332 量測、排程器縮鎖(依量測)、平台看門狗、RAM scrub、時間 / 空間分區 | admission / partition 模型、量測資料分析、fault-injection 模型 | 未開始 |
| P7 | user ABI、POSIX libc、工具、編輯器、dynamic linker | parser / 格式 / 相容性向量 | 未開始 |
| P8 | x86_64、UEFI | ABI / 解析器 / 格式核心 | 未開始 |

## 依賴與返工風險(和 Codex 文件逐項核對,無差異)

1. 裝置 / IRQ / 完成通知介面(原 P5a)提前到 P1:否則 P2 的 block IRQ、P4 的網卡 IRQ、PCI 與 USB 會各自長出不同的註冊與完成模型,之後統一成本高。範圍要窄;若查證現有介面已足夠,記下理由,不為抽象而抽象。
2. monotonic 時鐘要早於 poll timeout、alarm 與 I-332 量測,否則逾時與延遲的定義會分裂;wall clock 與 monotonic / 效能時鐘語義分開。
3. journal 必須先定好 flush / barrier 與斷電重播,才能做 root-on-disk;分割區不是第一版 root-on-disk 的前置(可先用整顆磁碟)。
4. page cache 與檔案 mmap 穩定之前不做 swap;swap 開工前重新證明「承諾必兌現」、配額、fork / COW、回收與 I/O 失敗的不變量。
5. 通用 socket 與 FD readiness 定好之前不做 TCP,否則協定狀態機會綁死在 UDP 專用的 FD 上。DHCP 依賴 UDP,不依賴 TCP。
6. 沒有量測證明瓶頸之前不拆 sched_lk。
7. I-333 目標硬體未定(使用者確認:先做模板),只阻擋硬體支援項目,不阻擋模板、host 模型與 QEMU 可驗證的抽象。

## 使用者開工門檻

本文件只定順序與分工,不授權開始任何階段。每一項:兩份設計審查 → 互給建議 → Codex 定版 → **使用者核准開工** → 實作。介面凍結(例如 P0 的 `include/i349_classify.h`)屬於設計的一部分,可以在核准前完成;程式實作要等核准。

## 和 Codex 文件的差異

目前沒有需要 Codex 決定的差異。P0 的交接介面細節(結構、列舉、理由碼、門檻參數)提案在 msg 250,待 Codex 確認凍結。
