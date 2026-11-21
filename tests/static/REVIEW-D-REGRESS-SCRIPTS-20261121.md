# 回歸腳本191變更：獨立審查（2026-11-21）

**分項結果**：preflight fail-fast/-k、configure/build失敗禁止回落舊ELF，以及獨立run目錄／repeat成功log保留可接受；整體runner仍有假PASS、timeout殘留與清理誤匹配缺口。本報告只評腳本可靠性，**不是RAMFS本體失敗結論**，也不否定root另有guest-marker核對的固定boot驗收。

只讀產品腳本；在/tmp複製原regress/qemu腳本，以假cmake、preflight、PowerShell、backend做必要mock。沒有啟動任何QEMU／編譯kernel／改產品。唯一真process測試是受控Python假backend，測後只清自己的PID/process group，確認gone。

## 1. 高優先：只核對rc可讓零客體輸出被宣告全通過（已重現）

regress.sh:136–138及repeat分支只看wrapper exit，沒有要求任何guest啟動／預期完成marker。隔離mock：fake backend每次直接exit0、完全不輸出，執行 `regress.sh -a riscv64 -q -r 1` 仍exit0，印4/4與1/4/8CPU各1/1通過、全部符合預期，7個guest log全為0 bytes。

i386更有具體exitcode混淆：qemu.sh:128把任何backend exit1映射0，無法從rc區分正常isa-debug-exit與QEMU一般啟動錯誤。第二個mock讓backend四次一律exit1且無客體輸出，`regress.sh -a i386 -q`仍4/4、exit0。真QEMU的錯誤訊息不是此測試的fixture，因此不宣稱特定QEMU故障已實測；但腳本明確缺少區分正常guest完成與generic exit1的證據條件。

修正條件：每case同時核對可接受wrapper rc與客體啟動／預期exit marker；selftest/memhog/bench還應要求各case完成marker，crash應核對預期客體失敗訊息而非只接受任何rc1/3。空log、啟動錯誤log、marker不符都必須失敗。沿用root獨立runner的marker原則即可，不必為此改RAMFS產品。不得用「檔案非空」替代正確marker。

## 2. 高優先：timeout沒有保證清掉子process（已用假backend重現）

所有呼叫使用 `timeout 60`，沒有kill-after上限。用真GNU timeout0.3跑原qemu.sh，backend僅是忽略SIGTERM的Python程式：wrapper如期124，但backend仍存活。qemu.sh並未保存／回收child PID；regress cleanup只查Windows Win32_Process，無Linux child回收路徑。這個native/Linux反例已重現，未聲稱本輪啟動Windows QEMU重現。

Windows方面則從程式可見cleanup只在整輪結尾與EXIT執行，case timeout後會繼續下一case；若native.exe未受WSL的timeout終止，可能先殘留到最後而不是每case結束即清除，佔用後續disk／host資源。這是未做Windows實測的靜態風險。

修正條件：每case保存自己的launcher/實際backend身份，TERM後有明確grace與KILL上限，在返回下一case前確認自身子程序結束；Linux process group與Windows PID要各自處理。只加timeout -k仍應驗證native.exe生命週期，不能假定WSL轉送涵蓋它。所有清理限定本次case/run，不得恢復全域killall。

## 3. 中優先：Windows cleanup不是精確name匹配（靜態確定，未實際停其他程序）

regress.sh:49用 `CommandLine -like '*$QEMU_RUN_TAG*'` 對qemu-system-*整段命令列做substring篩選，再Stop-Process。沒有保存實際啟動PID，也沒有解析 `-name` 參數作完整值比對。

按實際命名，若同一秒有run shell PID12與PID123，tag分別為 `welyos-run-20261121-143902-12`、`welyos-run-20261121-143902-123`；前者pattern會匹配後者完整tag，前者cleanup可選到後者。兩個同秒且數字prefix的run是此反例前提，不是「任何run一定誤停」。本輪PowerShell是只記錄命令的mock，**沒有真的啟動或停止Windows QEMU／別人程序**。case-log/path等其他CommandLine欄位偶含同tag也會被選，但不需靠這個額外假設即可展示prefix問題。

修正條件：優先記錄確切Windows PID及建立時間，清理前核對其image/path與本run身份；或至少解析-name並做完整相等，不能只靠substring。測試應同時放入本run、prefix兄弟run及完全無關run，後兩者不可被選。

## 4. log／run-info證據仍有缺口

- 正向：每次run用時間+PID獨立目錄，五個mock run均有不同路徑，舊run保留；repeat的成功log現在保留，包含smp1/4/8與序號。這是功能改善。
- safe-mode五次boot仍對同一 `$OUT/$a-safemode.txt` 使用 `>`，前四次成功log被覆寫。不能把目前「repeat成功log已保存」概括成「所有成功case證據都保留」。每階段應有step編號log。
- preflight輸出未tee進run目錄；default早退的run-info僅start無end/result（mock已見）。跑完的run-info也只有end，無總exit與逐case rc/timeout/cleanup結果，事後不能只靠run-info定位執行到哪裡。
- source只有short HEAD及dirty標記，沒有patch/hash、實際boot/bin/firmware/disk輸入hash、emulator/toolchain版本、完整env與逐case命令。這些不影響本輪fail-fast正確性，但不能將run-info當成完整可重現milestone manifest。
- 主要case仍用預設build/<arch>/disk.img，只有跨boot專項disk放進run目錄。因此輸出目錄隔離不等於disk/build資源隔離；目前owner序列執行時可用，不足以宣稱多runner可同時安全共用build。

## 已通過的mock範圍

| 情境 | 最終exit | cmake calls | backend calls | 結論 |
| --- | --- | --- | --- | --- |
| preflight失敗，default | 1 | 0 | 0 | fail-fast正確 |
| preflight失敗，-k | 1 | configure+build | 4 | 可繼續，但保留失敗，不洗成PASS |
| configure拒絕，已留舊ELF | 1 | configure | 0 | 不回落舊boot |
| build拒絕，已留舊ELF | 1 | configure+build | 0 | 不回落舊boot |
| 空輸出backend0，-q -r1 | 0 | configure+build | 7 | 假PASS缺口已重現 |
| i386空輸出backend1，-q | 0 | configure+build | 4 | generic1被正規化成成功 |

程式也檢查build後ELF非空，AArch64額外檢查實際.bin非空；qemu.sh把該架構ELF參數轉同名.bin的行為保留。這可拒絕缺檔，不證明binary與某source hash同時點；若build正常回成功，本輪不把一般incremental no-op誤當「已發現舊boot回落」。

## 證據、來源與限制

- `/tmp/welyos-regress-scripts-review-3_oq5n6s/results.json`：SHA-256 `7944be5a03a6c82a69b8d6118e2253d7fdfc298ccc0262b55647f5abb21c2be7`。同目錄保存隔離project、各stdout/stderr、fake命令紀錄、run-info與空guest logs；reproduce.py保留主要mock流程（i386補測另列results）。
- scripts/regress.sh SHA-256 `363f1afb7f08e28282a6dd677d633144b98dfeb25ebc7ba5e6b0f8e8cd466ba5`；scripts/qemu.sh SHA-256 `86820b075b1cfc10668d4a3ab1cb76f51d056051e3ebadd7d8d29d00ca62f1db`。閱讀前後相同。
- mock預檢與exit5 helper是受控stub，這裡不重新驗靜態gate規則或真host crash分類。沒有執行整套QEMU矩陣、Windows Stop-Process、真emulator timeout或故意干擾其他runner。
- 判定：腳本的fail-fast／不回落／repeat evidence改善可分項接受；「可靠核對guest結果、timeout後全清且不誤停、全case可追溯」仍open。RAMFS資料／metadata帳務本體與root獨立QEMU結果應另列，不以這些runner缺口一律否決或宣稱通過。
