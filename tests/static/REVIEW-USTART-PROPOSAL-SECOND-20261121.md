# ustart KOBJ 提案：第二位獨立審查（2026-11-21）

本次審查 `tests/static/proposals/tempobj-kobj-20261121.patch` 與 `probe_ustart_kobj_proposal.py`。沒有修改其他現有檔、套用產品或執行 QEMU。`git apply --check` 對本次產品基準成功。root所述三架構compile-only結果是root證據，本報告不冒稱自己執行。

## 結論

未在本次可見呼叫圖找到 ustart 自有domain參考／記帳的漏退或重退：提案可供owner整合，但**必須同步補強既有stack failure測試的故障位置證明**，否則舊QEMU輸出會變成只驗ustart入口quota拒絕，不能延用為stack rollback驗收。fork_start/frame不在此patch。

## 所有權與釋放順序

- kmalloc失敗沒有物件；mb_charge_object失敗時真helper會撤銷剛取得的domain參考、配額沒加成功，creator只kfree正確，不能再release一次。
- charge成功後st保存dom及實際kmalloc_size取整的kobj，獨立持有一份domain reference，不依賴隱含thread reference；不能省略成只存p再在釋放時查p->mem.dom。
- auto-stack配置／對映失敗、thread_prepare失敗均經ustart_release。auto-stack既有mem.mx整格rollback仍先完成，之後退ustart帳；supplied-stack不解除caller頁或slot。
- prepare成功後兩個現有caller（proc_add_thread、proc_create）均接著start，中間沒有可回失敗的步驟。user_thread_main先拷貝所有st欄位，再release arg，之後只用區域副本；正常、dying、killed三個出口之前都已退帳。
- ustart_release在kfree前保存dom/kobj，先釋放heap物件再mb_release_object（sub+put），不會在可能最後一次domain put後讀st或domain。heap與MB鎖分段，不新增80→75巢狀。若帳務錯誤mb_release_object仍執行put並回錯，提案記blackbox；此路徑是既有diagnostic政策，不是成功路徑不平衡。
- NULL domain保持不記帳；此分支在本次proposal host工具沒有直接案例，普通user proc正常持有非NULL domain。
- thread_abandon不清理arg。若未來新增prepare成功後可abandon的caller，必須明確退ustart；目前呼叫圖無此路徑，本patch不能被描述為通用arg destructor支援。

## 獨立重跑與工具強度

我方執行 `python3 tests/static/probe_ustart_kobj_proposal.py`：baseline 24次fixture情境exit0，consumer漏退／失敗漏退／固定1頁成本三突變分別以15／10／10捕捉，編譯成功、UBSan無診斷、changed_inputs=[]。證據 `/tmp/welyos-ustart-kobj-probe-txho5f61/results.json`，SHA-256 `8c83ceea47cc4a785317c39840eb5e24533bdbba46e792551e0c3d7d717b0fed`。

工具確實在隔離副本以fuzz=0套patch，再抽取改後ustart/stack helpers與真membudget charge/release/get/put/quota函式；沒有把預期帳務手寫代替真helper。兩種allocator大小成本、quota/ref失敗、第三頁alloc／第四頁map失敗、prepare失敗、supplied失敗、consumer/dying/killed、最後domain reference均有控制；24次包含重複成功控制，非24種獨立產品故障點。

限制：成功thread_prepare是queued arg fixture，沒有真thread KOBJ/domain引用；「原owner先put，ustart持最後reference」是釋放helper控制，不代表實際process/thread會失去其自身引用。heap/VM/PMM、mutex、blackbox與user entry皆fixture，沒有測真OOM、架構ABI或SMP。工具從proposal複本更新函式，但ustart fixture結構是手寫，不是抽取真struct；新增欄位順序本次人工核對相符。未測prepare成功後abandon、NULL domain、真正thread創建與完整proc_create lifecycle。它可以支持提案ownership，不能取代產品整合驗收。

## stack failure測試退化：具體修正條件

現 `proc_thread_failure_probe` 在proc.c:1160把limit設為原st.total，再呼叫proc_add_thread。新ustart charge位於任何auto-stack配置之前，成本至少1頁，因此每次都會提前拒絕。原 `user_stack_rollback_test` 只檢查成功數0及slots/pages前後相等，新路徑同樣滿足，卻沒有一次走到stack mapping或kernel thread_prepare。這是確定的覆蓋退化，不是產品錯誤退出新漏退。

建議owner選一種明確機制，驗收須有獨立到達點證據：

1. 最可靠是selftest專用、限定目標thread準備的一次性故障注入，在auto-stack初始4頁已映射、slot已claim且ustart已charge後，讓thread_prepare指定步驟回失敗；記錄「已完成stack4頁／進入指定prepare失敗點／未start」計數。不得把測試entry=0真正啟動。
2. 若仍用quota觸發，limit需足夠容納ustart實際kmalloc取整成本與本次新增PGTABLE成本，且不足kernel thread精確KOBJ。不能只把原limit+1：ustart結構／allocator尺寸與有無現成中間表都會影響成本；也不能用通過後回讀的產品總used倒推「必然到達」。需在鎖定測試目標狀態後，獨立計算／預建頁表或使用明確phase hook，斷言4初始頁完成且thread_prepare已呼叫、USTART quota/ref暫時存在。

每個round都要確認指定失敗點至少命中一次，回復後slots/pages與ustart/thread quota、domain refs回baseline；並有把stack rollback移除就會失敗的負對照。若只第1次能映射4頁，其餘卡在頁表quota，不能稱20次皆驗後段失敗。首次正確stage +後續入口拒絕應分開報告。部分第1–4頁失敗及並行deep fault仍屬另外的host/後續整合項，不能由此單case補齊。

## 綁定

| 檔案 | SHA-256 |
| --- | --- |
| proposal patch | 21f8f77159a356d2fd025cd2450efd9bd6e95097c5f174cbb59afb704eba91be |
| proposal probe | 3a99541c123e85bafd62278871f5c274145e0eaed9d68c735bec6bd210c63028 |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
