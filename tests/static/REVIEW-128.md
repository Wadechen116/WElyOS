# 第 46 步 baseline 交叉複核

日期：2026-11-19。來源 HEAD `9b5fcbb`。使用者指示繼續未完工作，並明確指出第 46 步重寫的 baseline 尚未重新交叉確認。本輪完成此項分配。

## 逐筆結果

`baseline-crosscheck-128.json` 記錄 **43 個不同的 finding ID**：

- Claude 第 46 步重新寫理由的 **12 筆**。
- Claude 補上新 close/fsync 契約理由的 **23 筆**。
- 另 **8 筆**重映射：mode_proc 的兩筆 close、disk_stage_test 的四筆 unlink、mode_fs 的兩筆 append write。

合計 37 筆 close、6 筆非 close。逐筆記錄來源位置、呼叫原文、當前函式指紋、舊 baseline 理由的 hash，以及本次獨立判定理由；並以新完整掃描確認 ID 與當前來源相符，不沿用舊 cross-check 的行號。所有相關來源及 baseline 在複核前後的 hash 一致。

逐筆接受各呼叫的例外範圍，**其中一筆必須改寫舊理由**：

`877f4646328777f65072c735`，user/init.c:254，mode_proc 中關閉 O_WRONLY 的 /bin/init。前面的 write 被 E_TXTBSY 拒絕，不代表 vfs_flush 會略過可寫描述符；原理由『close 只可能 EBADF』不能當一般契約。當前 self-test 的 /bin/init 由 initrd 載入 ramfs root，ramfs_ops 沒有 flush callback，所以這個特定 close 仍可忽略。若改為 WFS 或其他後端，必須重新審查。

另兩個理由細節已在複核紀錄收緊：mode_proc 的 saved 是額外 console fd，open 未明確檢查且後續未使用，不能聲稱所有無效 fd 都必然在別處被測出；mode_heap fd2 與 mode_pipe fd10 的 device 假設依賴 kernel-spawned self-test 的 fd_setup_console，不能移植到任意重導向執行環境。

核對新契約：sys_close 先 fd_remove，無條件 flush 後 drop reference，錯誤仍消耗 fd；vfs_flush 對唯讀直接回 0，pipe/socket/device 的非 durable flush 回 0。新清單沒有拿『唯讀』替 O_WRONLY 的 /bin/init 辯護，也沒有拿管線寫端當磁碟寫檔。

原 user/init.c 的 13 個可寫 close 已改為檢查結果：12 個轉換註解的位置加 mode_proc:208 的子行程輸出 close。cp/tftp 的目的 fd 以及 sh 的 keep 都有檢查 fsync 和 close；其餘 close_quiet 的暫存／失敗清理不被這次精確 ID 審查擴大認證。背景／停止／無法等待工作不提供完成後同步保證，仍依既有契約。

## 交付與驗證

交付 `patches/baseline-crosscheck-128.patch`：替上述 43 筆補 cross_checked_by/on/record，並只改寫上述 /bin/init 一筆 reason；不新增或刪除接受的 ID。`git apply --check` 通過。依檔案所有權交付補丁，**未直接修改 Claude 負責的 baseline**，也未刷新 reference。整合者可使用 `git apply tests/static/patches/baseline-crosscheck-128.patch`。

目前已提交產品原碼獨立複驗：

- shell matrix **369/369 PASS**，/tmp/welyos-shell-matrix-8rg63m1a，ASan/UBSan，changed_inputs 空。
- shell cleanup **15/15 PASS**，/tmp/welyos-shell-cleanup-03l9ztxh，UBSan，changed_inputs 空。
- checker **24 tests PASS**。
- 正常 static gate **PASS**，相對 HEAD 新 hard 0、新 style 0。
- 新完整 scan：/tmp/welyos-crosscheck-128-fast，97 accepted、0 未審 hard、0 stale、792 未審 style；source/tool changed 均空。此 all gate 因既有未審 style 而 exit 1，不宣稱全掃描零提示。

這是新契約下的來源交叉複核與現有模型複驗，沒有新增 QEMU 故障注入、共享 fd/concurrent writer 動態驗證，亦不是整個函式或全 OS 正確性證明。既有 analyzer 的 complexity/cppcheck 覆蓋限制仍見 REVIEW-124。無 kernel build/QEMU/commit/push。

原分配尚缺的 baseline cross-check 與產品整合後 shell 複驗均已交付；WFS 其他候選路徑仍屬未擴充動態驗證的候選，不列成本輪已驗證結果。
