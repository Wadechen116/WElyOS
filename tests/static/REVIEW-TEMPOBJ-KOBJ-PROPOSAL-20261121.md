# 暫時物件 KOBJ 提案：先交完整 ustart 子批

交付供產品 owner Claude 審閱，**未套用共享產品**：

- `tests/static/proposals/tempobj-kobj-20261121.patch`
- `tests/static/probe_ustart_kobj_proposal.py`

隔離實作目錄：`/tmp/welyos-ustart-proposal-qxk6530u`。patch 只修改
proc.c 與 proc_internal.h 的 ustart 路徑，不包含 fork_start/frame；檔名為整批
提案名稱，不能據此宣稱三類暫時物件全部已記帳。未 commit/push。

## owner、基準與變更

共享產品是 Claude owner、目前有未提交修改，HEAD
`4c04c3bb5de7ce36bbe2063f9cd5484fc9cc29ef`；不能只拿 HEAD 重建來套 patch。
本次先讀現版 hash，再複製至 /tmp、修改複本並產生差異；host 驗證另複製
現版並以 `patch -p1 --fuzz=0` 套用。產品輸入前後一致。

| File | Base SHA-256 |
| --- | --- |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |

Patch SHA-256：`21f8f77159a356d2fd025cd2450efd9bd6e95097c5f174cbb59afb704eba91be`。

`struct ustart` 增加自身持有的 dom/kobj；配置後以真 kmalloc_size（含新欄位
與allocator標頭）向上取整，呼叫現有 mb_charge_object。此操作成功同時得到
一份獨立 domain reference 與 KOBJ，失敗只 kfree 未記帳物件，不誤退帳。
所有後續失敗與 consumer 釋放統一走 ustart_release：先保存 dom/kobj，
釋放物件，再 mb_release_object（sub+put），帳務不符記 blackbox。

domain NULL 保留不記帳語意；正常 user proc 有 domain。不是使用 thread 的
隱含 reference：即使原 owner 先 put，ustart 自有 reference 仍保證 domain
活到它被消費。成功交接之後 creator 不再釋放，user_thread_main 在讀出
ustart 副本後立刻歸還，早於 dying/killed 分支，因此三條出口不會漏退。

| 路徑 | ustart 責任歸屬 |
| --- | --- |
| kmalloc失敗 | 無物件無charge |
| charge quota/ref失敗 | creator只kfree；charge函式已撤銷暫取得reference |
| auto-stack配置/對映失敗 | creator ustart_release；stack既有交易負責回退頁/slot |
| thread_prepare失敗，auto或supplied stack | creator ustart_release；不碰supplied buffer |
| prepare成功 | ownership隨arg交給準備好thread；現有caller接著只start |
| user_thread_main正常/dying/killed | consumer在任一分支前 ustart_release |

現版 user_thread_prepare 的成功呼叫者只有 proc_add_thread、proc_create，兩者
都接著 start；若未來新增「prepare成功後abandon」呼叫者，必須先接收/保留
arg cleanup ownership，再 ustart_release，因 thread_abandon 不會替caller
清理arg。此提案不擴大thread API、不引入自動arg destructor。

## 可重跑驗證

執行：`python3 tests/static/probe_ustart_kobj_proposal.py`。
抽取隔離 patch 後的真 stack helpers、ustart_release、user_thread_main、
user_thread_prepare；加上真 membudget get/put/quota/charge/release 函式。

24次測試通過：兩種 kmalloc 實際大小向上取整成本（1頁／2頁），各檢查
ustart配置失敗、quota不足、domain refs飽和、第三頁PMM失敗、第四頁map失敗、
kernel prepare失敗、supplied stack prepare失敗、成功consumer控制、dying、
killed、原owner先put而consumer持最後domain reference、另一成功控制。
24包含重複成功控制，不是24個不同產品故障點。

三突變皆被指定出口抓到：consumer只kfree漏退帳→15；失敗只kfree漏退帳→10；
固定算1頁漏實際取整→10。baseline exit0、UBSan clean、changed_inputs=[]。
證據：`/tmp/welyos-ustart-kobj-probe-zncckjnq/results.json`，同目錄保留套用後
產品複本、四份C與執行檔。工具每次會產生新/tmp目錄與新的來源hash。

fixture 替代heap、PMM/VM、kernel scheduler、thread_prepare、黑盒子與進入
user mode；成功consumer以setjmp/longjmp保留不返回契約。真thread的KOBJ另由
既有生命週期測試覆蓋，本工具沒有將它混算為ustart。這不是產品build、QEMU、
SMP、真正OOM或架構ABI驗收；owner套用後仍須原生三架構build/selftest。

**必須調整既有stack測試的故障位置**：proc_thread_failure_probe 目前將limit設為
st.total。此提案在stack之前新增ustart charge，所以套用後測試會先在ustart
被拒絕，完全不觸stack；即使slots/pages前後不變，也不能繼續宣稱驗了stack
對映完成後的thread_prepare失敗。owner須提供明確thread_prepare故障注入或
計算足夠通過ustart+stack頁表而不足kernel thread成本的quota，且記錄確實到達
指定故障點。這是測試強度限制，不能為了讓舊測試走原路而漏掉ustart記帳。

## 剩餘 fork_start/frame 子批

尚未實作。建議每個物件明確持有子domain reference與各自kmalloc_size成本；
在 pmem_fork_domain 成功後、thread_prepare 之前記帳，所有會失敗的charge
必須在 pmem_fork_space 前（後者成功後父PTE已改COW）。

錯誤釋放要區分「domain還沒建立」「第一物件charge成功第二個失敗」「thread
prepare失敗」「fork_space失敗後thread_abandon」；一律先保存賬務欄位，
釋放物件，再放其domain reference，不能pmem_release後再從c->mem.dom取值。
consumer fork_child_main 先保存fork_start，frame複製到自己的kernel stack後
才釋放frame，兩物件要在dying/killed檢查之前完整歸還。不得在fork_space
成功後補charge，也不得把它們暫借exec pending信用。

下一個完整子批應對這些錯誤點逐一注入，並核對父PTE/票仍不變、無pid發布、
兩物件自有reference歸零；不能用本次ustart通過替代fork_start/frame驗收。
