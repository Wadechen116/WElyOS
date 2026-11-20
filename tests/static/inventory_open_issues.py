#!/usr/bin/env python3
"""Inventory the historical open I-xxx rows without changing the authoritative issue file.

This is triage, not proof that every requested feature has been implemented.
Every original row stays represented; compound issues are never silently closed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]

GROUPS = {
 'platform': dict(owner='Claude', reviewer='Codex', paths=['lib/fdt.c','arch/riscv64/timer.c','arch/i386/smp.c','arch/aarch64/arch.c'],
   next='先建立可配置 DTB/Multiboot/裝置模型的最小輸入，再替換對應寫死參數；保留 QEMU 三架構啟動回歸及失敗裝置對照。'),
 'memory': dict(owner='Claude', reviewer='Codex', paths=['kernel/pmm.c','kernel/kmalloc.c','kernel/vm.c','kernel/proc_mem.c'],
   next='先定此項資源/對映生命週期與鎖順序；以真函式主機案例重現最小邊界，再交產品修正與撤回修正的負對照，最後跑多核 QEMU。'),
 'scheduler': dict(owner='Claude', reviewer='Codex', paths=['kernel/sched.c','kernel/sync.c','kernel/spinlock.c','kernel/runq_ring.c'],
   next='以目前 A2 基準記錄現況，再針對本項建立確定性交錯或延遲案例；修正後驗證同級公平、鎖順序、取消/喚醒及 1/4/8 CPU 行為。'),
 'console': dict(owner='Claude', reviewer='Codex', paths=['kernel/console.c','arch/i386/vga.c','kernel/proc_signal.c'],
   next='用實際序列輸入/控制鍵建立本項最小互動案例；保留前景/背景權限與模式還原，修正後以等待提示的 QEMU 按鍵測試驗證。'),
 'shell': dict(owner='Codex', reviewer='Claude', paths=['user/sh.c','tests/static/probe_shell_features.py','tests/static/probe_shell_matrix.py'],
   next='保留 shell 現有作業/描述符生命週期，針對本項加腳本或按鍵案例，交隔離測過的產品補丁給 Claude 套用及三架構 QEMU 驗證。'),
 'user-tools': dict(owner='Codex', reviewer='Claude', paths=['user/top.c','user/libc.c','user/ulib.h'],
   next='以本項實際命令列輸入與失敗回傳建立主機測試，實作後核對輸出、結束碼及資源釋放；Claude 整合 QEMU 驗證。'),
 'process': dict(owner='Claude', reviewer='Codex', paths=['kernel/proc.c','kernel/proc_fs.c','kernel/proc_signal.c','kernel/syscall.c','include/uapi/proc.h'],
   next='先定本項對使用者可見的系統呼叫/生命週期契約及失敗回滾；Codex 做真函式邊界/並行反例，Claude 實作與多核整合。'),
 'storage': dict(owner='Claude', reviewer='Codex', paths=['kernel/wfs.c','kernel/vfs.c','kernel/bcache.c','kernel/blkfault.c'],
   next='用 RAM 區塊裝置或斷電映像重現本項，記錄磁碟前後差異；先定持久性/格式相容與回滾契約，再以同一故障案例驗證修正。'),
 'network': dict(owner='Codex', reviewer='Claude', paths=['kernel/net/ipv4.c','kernel/net/udp.c','kernel/netif.c','tests/host/net/test_net.c'],
   next='先建立本項封包/計時事件的主機案例，確認不配置記憶體且有界；產品補丁由 Claude 整合到驅動與 QEMU 網路測試。'),
 'reliability': dict(owner='Claude', reviewer='Codex', paths=['kernel/blackbox.c','kernel/panic.c','kernel/main.c','user/supervise.c','user/vote.c'],
   next='先列明本項故障模型及哪些裝置仍可用，做錯誤注入與正常對照；把診斷成功和真正復原分開驗證，不以訊息取代復原證據。'),
 'structure-tests': dict(owner='Codex', reviewer='Claude', paths=['scripts/static/check.py','scripts/arch-check.sh','CMakeLists.txt','tests/static/REVIEW-129.md'],
   next='先重跑涉及本項的現有工具並保留失敗；若拆分則凍結介面、比較真函式與鎖前提，再補編譯器/故障負對照，由 Claude 整合。'),
}
# Current tree can contain additional/new files; only existing references enter
# the manifest. These references are inspection locations, not proof by existence.
SETS = {
 'platform': '1 2 3 9 10 11 30 34 78 79 80 124 125 126',
 'memory': '7 8 12 18 27 28 29 37 39 52 53 59 90 129 130 131 132 133 134 136 140 209 210 240 241 242 243 244',
 'scheduler': '15 16 19 21 22 23 33 35 43 44 62 63 66 67 68 69 70 71 72 81 82 83 84 86 87 88 91 92 143 151 282 283 284 285 286 303',
 'console': '36 38 60 76 77 89 116 117 120 127 161 162 163 170 175 199 200',
 'shell': '119 122 138 148 157 166 176 177 178 179 206 219 311 314 318 319 321',
 'user-tools': '171 193 194 195 196 197 198 203 212 213 222 226',
 'process': '106 110 113 128 135 147 149 150 155 158 160 173 174 204 214 215 216 217 218 220 232 233 234 235 236 237 238 245 323',
 'storage': '145 146 101 103 112 180 181 182 183 184 185 186 187 188 189 191 202 205 207 221 224 225 227 228 229 230 231 266 267 268 269 270 271 272 273 277 307 308 309 312 313 315 316 317 320',
 'network': '246 247 249 250 251 252 253 254 255 256',
 'reliability': '144 223 257 258 259 260 261 262 263 264 265 274 275 276 278 279 280 287 288 289 290 291 292 293',
 'structure-tests': '141 142 294 295 296 297 298 299 300 301 302 304 306 310 322 324 325 326 327 328 329',
}
# Evidence-based candidates for the document owner, not automatic issue edits.
FIXED = {
 72: ('tests/static/probe_runq_overflow.py; tests/host/runq_overflow_probe.c; /tmp/welyos-runq-overflow-pz85rp82; /tmp/welyos-runq-overflow-3w35erfu; /tmp/welyos-runq-overflow-wxfaovli', '真ovf_add/claim的三種受控交錯3/3PASS，其中link後check前是真pthread並行；移除producer-check與claim-unlink各抓到對應殘留。不是全部runqueue/SMP證明。'),
 15: ('kernel/sched.c:thread_join/thread_sleep_until; kernel/sync.c; include/sync.h', '同步機制與 join 已存在，sleepers 按 wake_tick 排序；舊項自己也說第11步已補。'),
 19: ('include/spinlock.h; include/sync.h; kernel/spinlock.c:order_check; kernel/sync.c; docs/ISSUES.md:N-001', '第50步有 spinlock/mutex rank 檢查與三架構負對照；不是一般任意依賴圖的死結偵測器。'),
 120: ('kernel/console.c:console_set_mode/console_input; user/top.c:main; include/uapi/proc.h:CON_RAW', '既有 raw/noecho 介面與 top 使用它；原本沒有raw的敘述已過期。'),
 301: ('kernel/proc.c; kernel/proc_signal.c; kernel/proc_mem.c; kernel/proc_fs.c; kernel/syscall.c; tests/static/REVIEW-129.md', '第47步已拆分，17項seam測試本輪修復後再PASS；不表示後續I327的小型重構也完成。'),
 302: ('tests/static/probe_boot_boundaries.py; tests/host/sched/periodic_probe.c; /tmp/welyos-boot-boundaries-ibm50vyi; /tmp/welyos-periodic-abj3p47j', '本輪先證實編譯失敗，再修MUTEX_INIT與sched_tick介面；boot16案與periodic64邊界/20000記帳PASS，含tiny63-66/seq-wrap。'),
 308: ('tests/static/probe_wfs_failures.py; tests/host/wfs/failure_probe.c; kernel/bcache.c; /tmp/welyos-wfs-failures-zj6u10ij', '既有主機fixture獨立編譯真bcache.c，直接測sync-write/evict-write並注入RAM裝置失敗；不能稱快取完全沒有測試，尚非完整快取並行覆蓋。'),
 311: ('tests/static/probe_shell_matrix.py; tests/host/shell/matrix_probe.c; /tmp/welyos-shell-matrix-s76nqhbq', '1/2/3 stages、四類redirect、每次dup2等故障及指定雙故障共369案PASS；不擴張為I321的真子行程/SMP全組合。'),
 313: ('kernel/syscall.c:sys_close/sys_fsync; kernel/vfs.c:vfs_flush; docs/ISSUES.md:N-003; /tmp/welyos-shell-cleanup-30_wczxc', '可寫檔close傳回flush錯誤且仍消耗fd，fsync及使用者失敗路徑已有host與Claude回報QEMU證據。'),
 314: ('kernel/test/io_test.c:disk_user_stage_test; docs/ISSUES.md:N-003; tests/static/probe_shell_cleanup.py', '第50步QEMU磁碟12-15已有cp/sh/tftp writeback失敗測試與正常對照；shell cleanup15案目前可跑。QEMU結果為Claude來源。'),
 319: ('tests/static/probe_shell_cleanup.py; tests/host/shell/cleanup_probe.c; /tmp/welyos-shell-cleanup-30_wczxc', '替身已有fsync/close，wait-interrupted/always-interrupted/io-error/continue-denied皆PASS，不再只有閱讀證據。'),
 320: ('tests/static/probe_wfs_failures.py; tests/static/probe_wfs_dir_errors.py; docs/ISSUES.md:N-006', '兩個真函式故障fixture涵蓋列出的bitmap/bmap/truncate/dir/alloc/readdir/check路徑；不宣稱所有未來I/O分支已證明。'),
 322: ('tests/static/baseline-crosscheck-128.json; tests/static/baseline-crosscheck-129.json; tests/static/baseline.txt; review:codex:20261120-001-n020', '第46步重對應已有128交叉審查，第47步129，第50步24筆N020完成並整合。'),
 325: ('kernel/proc_internal.h:PROC_OWNER_SIG/PROC_OWNER_MEM/PROC_OWNER_FILES; scripts/arch-check.sh; docs/ISSUES.md:N-007', '目前以不同編譯可見欄位名稱強制owner界線，並有10個實際編譯正負案例；不宣稱C指標/cast不能繞過。'),
 329: ('tests/static/probe_memory_races.py; tests/static/probe_proc_split.py; tests/static/probe_seu_race.py; /tmp/welyos-proc-split-s1_uiqgn; /tmp/welyos-memory-races-zk16fphz; /tmp/welyos-seu-race-jdsr2uue', '本輪修proc split的mem.mx模型後17PASS，memory9/9與SEU確定性交錯PASS；原路徑/介面失效問題可結案。'),
}
PARTIAL = {
 33: ('kernel/timer.c; kernel/sched.c:sched_tick; tests/host/sched/periodic_probe.c', '第48步charge=0已修補記tick扣片，第50步修ticket不公平；模擬器中斷延遲仍存在，改寫敘述避免保留舊3倍判準。'),
 81: ('kernel/smp.c; kernel/vm.c; arch/i386/mmu.c', 'TLB shootdown與kernel頁表切換已在第18步；PAE多次寫入問題仍須獨立驗證，不能整條結案。'),
 117: ('kernel/console.c:may_read/console_set_fg', '前景群組及背景讀取SIGTTIN已存在；單一console/session的限制仍在。'),
 119: ('tests/static/patches/shell-n005.patch; tests/static/probe_shell_features.py', '本輪補游標/歷史/Tab並有host與突變負對照，待Claude整合；UTF8仍按byte編輯，不能整條結案。'),
 186: ('kernel/vfs.c:vfs_fsync/vfs_flush; kernel/syscall.c:sys_close', '現在另有fsync與可寫close錯誤傳遞；普通write仍不保證durable，原句需要更新。'),
 227: ('kernel/blkfault.c:blk_fault_set_write_from/fault_rw', '第50步可以指定LBA起點範圍；仍沒有通用nth操作故障與每一種partial write驗證。'),
 283: ('kernel/sched.c:sched_tick; kernel/timer.c; tests/host/sched/periodic_probe.c', 'charge=0不再把補記計入目前執行緒；小於tick的取樣誤差仍存在。'),
 297: ('kernel/proc.c; kernel/proc_mem.c; kernel/proc_fs.c; kernel/proc_signal.c; kernel/bcache.c; kernel/test/', 'proc/main拆分與快取獨立已完成；中斷/裝置註冊及console/process依賴未完成，必須分拆子項。'),
 304: ('tests/static/probe_shell_matrix.py; tests/host/libc/stdio_probe.c', 'dup2故障已有369案例；不能據此宣稱所有工具短寫與sysinfo失敗皆涵蓋。'),
 310: ('docs/ISSUES.md:N-002/N-009/N-016; scripts/regress.sh', '計時/公平性已找到真缺陷並修正；exit5仍交Claude A1調查，未重現前不得宣稱已修。'),
 312: ('tests/static/probe_wfs_failures.py; tests/host/wfs/failure_probe.c', '本輪18/18涵蓋bitmap/tail read；完整GCC analyzer本輪未重跑，不把複合項整條結案。'),
}
BOUNDARY = {
 38:'#DF不只代表stack overflow；保留獨立診斷是正確行為，不能把所有DF都冒稱stack overflow。',
 43:'semaphore是計數/通知，沒有唯一owner；優先權繼承必須有明確被等待擁有者，保護資料應用既有mutex。',
 66:'ring固定容量有overflow串列作後備，容量64不是工作數64的功能上限；保留溢出與公平性測試，效能需求由基準決定。',
 67:'分散runq加工作竊取沒有跨CPU全域enqueue序；若要全域FIFO須引入全域序列化契約，不能誤當現有介面的承諾。',
 69:'highest只用作重新排程提示，取出仍核對真實佇列；過期提示的代價應量測，不等於遺失工作。',
 135:'kernel不直接解參考user VA而走受保護copy服務；未知kernel fault目前是不可恢復錯誤，恢復需要明確fixup表不能任意重試。',
 138:'shell可用spawn完成既有建立/redirect契約；改fork+exec會增加快照與quota成本，沒有必要僅為形式替換。',
 141:'MAX_CPUS=8低於32位mask容量；在擴充CPU上限前32位遮罩足夠，擴充時必須有編譯期上限檢查。',
 144:'QEMU數據必須標為QEMU；不能以程式補丁把模擬數據變成實機效能保證，實機需另有測試平台。',
 149:'spawn只繼承0/1/2是防止管線多餘寫端存活的明確契約；額外fd可由fork傳遞，file-actions擴充屬另加能力。',
 158:'終止在安全的kernel checkpoint交付，不能任意中斷並釋放正在使用的kernel物件；量測延遲而不是無條件強殺核心。',
 160:'由kernel建立的啟動/自測工作由kernel等待是明確所有權，不需要捏造使用者父行程；須驗證回收。',
 173:'sleep使用絕對deadline；暫停不等於讓整個單調時鐘停住。若要暫停剩餘時間須另加相對timer契約。',
 174:'致命/終止訊號不應因停止狀態無限擱置；若要改交付時機需區分不可忽略與可處理訊號，不宜一概延遲。',
 198:'top是被觀察系統的一部分，記入CPU才反映真消耗；應量化觀測開銷，不能隱藏它的CPU時間。',
 215:'目前一般訊號是pending位元集合而非事件佇列；若需要逐次計數則新增queued/realtime signal API，不能默認現有bitset保存多次。',
 218:'E_INTR讓呼叫者決定重試，能避免有副作用的操作被盲目重做；若加自動重啟須逐syscall定義可重試階段。',
 236:'既有fd代表已授予的存取權；即時撤權是新的全域撤銷契約，不能只在每次write補一個不一致的路徑檢查。',
 245:'CAP_ADMIN明確允許改quota；要不可逆限制應先drop capability，不能同時保留admin且宣稱限制不可調。',
 254:'目前ping透過受限ICMP API，不提供任意raw frame；是否需特權和限速是政策，不能只因與另一OS不同判bug。',
 262:'主要磁碟失效時無法保證往同一磁碟寫panic紀錄；若要獨立保證須增加獨立儲存故障域。',
 269:'CRC的檢查碼與資料同在磁碟，目標是意外損壞偵測；蓄意竄改要可信key與認證格式，不能把CRC稱為防竄改。',
 270:'軟體校驗只能覆蓋有冗餘與檢查點的記憶體；任意暫存器/指標bit flip不能靠區塊CRC保證，須ECC/獨立硬體故障模型。',
 280:'QEMU主機可任意暫停guest，1ms tick與全域鎖不構成hard-real-time上界；只能提供明確量測分布，不能宣稱硬期限保證。',
 285:'RT目前是thread層級的可選能力，不對整個process默認放大配額；fork自動繼承會破壞admission，需重新申請。',
 287:'三份相同程式/輸入的共同錯誤可一致輸出錯答案；增加同程式副本無法消除此故障域，須不同實作或外部規格。',
 288:'單一軟體voter本身是單點，不能靠自我比較證明免於任意自身故障；獨立voter屬新故障域。',
 289:'同kernel上的三process不能隔離kernel/硬體共同故障；要隔離必須多機/獨立kernel，非目前單guest產品能承諾。',
 290:'vote需要可重複且無外部副作用的命令；對time或write命令盲目執行三次會改語意，應保留使用範圍。',
 291:'輸出相同表示本次可觀測結果一致，不表示所有內存bit都一致；記憶體修復是另一份冗餘狀態契約。',
 292:'有界輸出是防止無窮輸出耗盡voter記憶體；超限應明確失敗，三重執行的時間/空間代價不能憑空消失。',
 293:'user-space injection刻意不允許任意kernel覆寫；若要kernel fault testing需隔離實驗且不能當一般使用者API。',
 296:'kernel與rootfs目前同批建置，沒有跨版本binary相容承諾；新增版本機制前不可宣稱支援任意舊binary。',
 315:'fsync成功的持久性與整筆操作斷電原子性不同；後者需journal/COW/transaction，不能在fsync註解直接保證。',
 317:'額外sync是shell承諾資料持久化的成本；若要移除須重新定義成功語意，先以A2量測才能討論批次同步。',
 324:'生命週期與signal/memory的雙向協作本身不證明bug；盲目改callback會藏起caller鎖和借用指標，應先明列契約。',
 328:'參數驗證屬於擁有該狀態的模組是可行設計；需消除重複/缺漏與明列邊界，不能只為函式前綴集中所有邏輯。',
}
DUPLICATES = {30:2,53:18,59:90,60:36,63:82,76:89,77:36,89:36,116:170,112:188,132:188,
              147:155,166:177,178:157,274:263,275:262,303:282,326:327}
# Related duplicates can retain unique remainder; never delete their original text.
SPECIAL_NEXT = {
 7:'設計頁對齊extent所有權與空extent歸還PMM；反覆大配置/全部free後頁數須回baseline，保留碎片仍在使用的對照。',
 12:'加入allocator尾端canary及檢查時機；寫過配置長度1byte須被抓到，合法最後byte與相鄰alloc/free不可誤報。',
 27:'建立安全的page-table替換/暫存執行區契約，再按AArch64 break-before-make執行與實機驗證；QEMU通過不能代替。',
 37:'比較三個toolchain的stack probing支援；以跨保護頁大frame反例證明有probe，不能僅加入不支援旗標。',
 72:'在overflow enqueue插入確定性hook觸發steal/reprio交錯；節點不得遺失/重複，撤回同步修正必須失敗。',
 90:'先以32bit輸出反組譯及並行觀察證明PAE leaf publish順序，再交atomic/invalid-first補丁並驗證NX不瞬間放行。',
 91:'定義thread affinity mask與離線drain契約，RT admission及TLB目標不能遺漏離線中的CPU。',
 106:'先凍結其他user threads及syscall refs，建立exec commit point；失敗恢復舊映像，成功後其他舊執行緒不可再執行。',
 119:'等B1三架構互動驗證，再補UTF8 grapheme/顯示寬度編輯的專門案例；不可將byte-editor算作已解Unicode。',
 122:'新增明確-e選項，測中途失敗立即停止和預設繼續兩種行為，避免無條件改變既有script語意。',
 179:'增加shell最近background pid的$!，確保job保存後發布；腳本以它STOP/CONT/WAIT實際子行程。',
 191:'由宿主wrapper以每映像排他鎖覆蓋整個QEMU生命週期；兩個程序競爭同映像時第二個必須明確失敗。',
 202:'設計rename replace與跨目錄transaction/鎖序；並行lookup只能看到舊或新名稱，斷電重掛亦不可丟失兩端。',
 214:'加入blocked signal mask與handler期間自動遮罩/恢復；pending不丟失，sigreturn不能寫入特權狀態。',
 217:'分拆CHLD/SEGV/PIPE/ALRM各API與預設動作；先測E_PIPE與SIGPIPE共存、SEGV handler返回後重fault的策略。',
 228:'在驅動register讀取層注入永不完成裝置，驗證timeout有界、資源回收與下一要求能否恢復。',
 234:'分拆sticky/umask/ACL/setuid，先實作sticky與umask最小安全案例；禁止以全面提高權限代替。',
 240:'把stack/映像/頁表/ramfs消耗納入可歸屬的reserve或全域上限；壓力下受控拒絕，kernel不能先OOM。',
 246:'先界定TCP子集狀態機與重傳timer，主機測重複/亂序/遺失/窗口邊界，再整合socket stream。',
 252:'加loopback接收分派且避免在持網路鎖時遞迴拿同鎖；本機ICMP/UDP主機測試與QEMU正常/拒絕對照。',
 267:'重掛前備份損壞映像，證明不能把壞資料重新計算CRC後當可信；設dirty-mount復原標記與可信metadata規則。',
 294:'定義syscall名稱/參數/指標方向schema，產生user wrapper與kernel dispatch原型，三架構編譯及負型別案例把關。',
 295:'把方便巨集移入既有uapi共用header，不改wire格式；移除兩份後兩端常值及網路host測試必須相同。',
 298:'加WELYOS_SELFTEST build選項並做on/off三架構link；off不可殘留測試符號，on的完整regress不可少。',
 304:'新增真ps/top/sysinfo失敗與write短寫的host入口，核對exit/messages，不能只引用shell dup2模型。',
 306:'逐筆按caller契約審風格提示並記來源hash；不以加入空檢查或批次接受降低計數。',
 318:'讓job保有獨立輸出file reference並在reap/fg/wait完成時fsync+close一次；多背景工作、stop/resume和失敗負對照。',
 321:'建立真host child/pipe內容與STOP/CONT/背景互動整合測試；另做多故障組合，不把fd模型當SMP執行。',
 323:'同一file經dup/fork共用時並行write/fsync的確定性交錯，核對file position、完整資料、持久性和close次數。',
}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, default=ROOT/'tests/static/open-issues-inventory.json')
    a=ap.parse_args();root=a.source.resolve()
    source=(root/'docs/ISSUES.md').read_text()
    mapping={int(x):g for g,seq in SETS.items() for x in seq.split()}
    rows=[]
    pattern=r'^\| (I-(\d+)) \| 待處理 \| (.*?) \| (.*?) \| (.*) \| (.*?) \|$'
    for line,l in enumerate(source.splitlines(),1):
        m=re.match(pattern,l)
        if not m:continue
        iid,num,domain,origin,content,old_follow=m.groups();num=int(num)
        if num not in mapping:raise ValueError(f'unassigned {iid}')
        group=mapping[num];meta=GROUPS[group]
        row=dict(id=iid,original_line=line,domain=domain,origin=origin,requirement=content,
            group=group,owner=meta['owner'],reviewer=meta['reviewer'],
            state='retained-open',evidence_kind='inspection-locations-not-validation',
            inspection_paths=[p for p in meta['paths'] if (root/p).exists()],
            next_action=SPECIAL_NEXT.get(num,meta['next']),
            acceptance='針對 requirement 的正/負案例、修正後同案例通過；由 owner 附證據，不以盤點本身標完成。')
        if num in FIXED:
            refs,reason=FIXED[num];row.update(state='recommend-resolved',evidence_kind='code-and-existing-test-evidence',evidence=refs,reason=reason,
                next_action='Claude核對證據後更新原條目後續；保留適用範圍，不擴張為整個子系統驗證。')
        elif num in PARTIAL:
            refs,reason=PARTIAL[num];row.update(state='partial',evidence=refs,reason=reason)
        elif num in BOUNDARY:
            row.update(state='boundary-rationale-proposed',reason=BOUNDARY[num],next_action='Claude審核此具體契約理由並在ISSUES寫明保留的使用範圍；這不是功能已實作。')
        if num in DUPLICATES:
            row['related_to']=f'I-{DUPLICATES[num]:03}'
        rows.append(row)
    assert len({r['id'] for r in rows})==len(rows)
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    result=dict(head=head,source_sha256=hashlib.sha256(source.encode()).hexdigest(),
      scope='all original open I-xxx rows; triage does not implement retained requirements',
      counts=dict(Counter(r['state'] for r in rows)),owner_counts=dict(Counter(r['owner'] for r in rows)),
      total=len(rows),rows=rows)
    a.out.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
