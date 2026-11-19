# 使用者程式 unchecked-error 逐筆分類（Codex，2026-11-18）

共122筆：benign 91、real 31。未寫入 baseline；benign 是限目前契約的逐筆建議，非批量接受。

real 表示錯誤／短寫會遺失或必要 setup 未檢查；不代表每一項都已有故障注入實證。selftest robustness 是測試本身的失敗回報／等待風險，與產品資料錯誤分開。

sys_close 現行只回 EBADF，核心的 void closed callback 會吞 flush 錯誤；單純檢查 close 無法建立持久化保證。這是 API 限制，不將所有 close 誤判成已能回報磁碟錯誤。

## real

| ID | 位置／函式 | 影響 | 判讀 |
| --- | --- | --- | --- |
| `d55a796e3b80473391e3a8ab` | user/echo.c:21 main | production | stdout 可能是已無讀者的管線或磁碟；負值／短寫被忽略，仍回傳原 argc。應保留成功時既有測試退出碼，但回報輸出錯誤。 |
| `86df970685f32aefaef573c2` | user/init.c:486 mode_signal | selftest robustness | 此處重新 fork_sleeper 後未先驗證 a>0，負錯誤碼可能被 kill 當成群組 PID；且後續 wait 可能無限等未終止的子行程。應先檢查建立與 kill。 |
| `69ef8e4f301a3ac2293b8ee2` | user/init.c:1296 mode_sighandler | selftest robustness | 未檢查 sysinfo 就讀取自動變數 t0/t1 的 uptime；copy_to_user 若失敗可能比較未初始化資料，須先檢查取樣成功。 |
| `e2a6da0bffb8e5f7529c9f97` | user/init.c:1298 mode_sighandler | selftest robustness | 未檢查 sysinfo 就讀取自動變數 t0/t1 的 uptime；copy_to_user 若失敗可能比較未初始化資料，須先檢查取樣成功。 |
| `df8898a08a3ced4cc1885fa5` | user/init.c:1333 mode_sighandler | selftest robustness | 該段 pid=fork 後未先排除負值，kill 會把負錯誤碼解作進程群組；部分目標本來無限睡眠／yield，送訊號失敗後的阻塞 wait 也可能卡住。須檢查 pid>0 及必要的 kill 成功。 |
| `715cae79107950db0c64e001` | user/init.c:1337 mode_sighandler | selftest robustness | 該段 pid=fork 後未先排除負值，kill 會把負錯誤碼解作進程群組；部分目標本來無限睡眠／yield，送訊號失敗後的阻塞 wait 也可能卡住。須檢查 pid>0 及必要的 kill 成功。 |
| `7284ac402b49f6b9abfb52a0` | user/init.c:1349 mode_sighandler | selftest robustness | 該段 pid=fork 後未先排除負值，kill 會把負錯誤碼解作進程群組；部分目標本來無限睡眠／yield，送訊號失敗後的阻塞 wait 也可能卡住。須檢查 pid>0 及必要的 kill 成功。 |
| `f1160a6ee7fc5b73f1d525d9` | user/init.c:1362 mode_sighandler | selftest robustness | 該段 pid=fork 後未先排除負值，kill 會把負錯誤碼解作進程群組；部分目標本來無限睡眠／yield，送訊號失敗後的阻塞 wait 也可能卡住。須檢查 pid>0 及必要的 kill 成功。 |
| `a1e146b04423aa4a3033aeb7` | user/init.c:1400 mode_sighandler | selftest robustness | 該段 pid=fork 後未先排除負值，kill 會把負錯誤碼解作進程群組；部分目標本來無限睡眠／yield，送訊號失敗後的阻塞 wait 也可能卡住。須檢查 pid>0 及必要的 kill 成功。 |
| `2f792f824077e54045242c2d` | user/init.c:1960 mode_stop | selftest robustness | 必要的 SIGCONT 失敗後直接阻塞 wait，子行程仍可能停住；檢查 kill 結果可直接回報 setup 錯誤而非整輪測試逾時。 |
| `b989612d3c0f624b0cf5cc6f` | user/init.c:1982 mode_stop | selftest robustness | 孫行程 sys_fork 結果 gc 未先檢查>0 就傳給 kill；失敗負碼會變群組 PID，可能送錯目標並破壞 orphan 測試。 |
| `fedf7586b9e3a8942ed27625` | user/init.c:2146 mode_pipe | selftest robustness | 保存／復原 stdout 是避免父端持有管線寫端的必要動作；若 SAVE 或 restore dup2 失敗，父端接著的 read 可能因自己的寫端仍開著而永遠無EOF。應直接驗證。 |
| `97532b1bafdcfb7f9c031159` | user/init.c:2151 mode_pipe | selftest robustness | 保存／復原 stdout 是避免父端持有管線寫端的必要動作；若 SAVE 或 restore dup2 失敗，父端接著的 read 可能因自己的寫端仍開著而永遠無EOF。應直接驗證。 |
| `d29632151dac0a00d0498f9c` | user/libc.c:164 out_char | production | 格式化緩衝區滿時忽略 write 負值／短寫便把 len 清零，永久丟失未寫出的字元；後續 printf 仍回成功字元數。 |
| `9ec373c28058407bac6c5511` | user/libc.c:347 vfdprintf | production | 最後一段 write 失敗／短寫仍回 o.total；呼叫者不能得知 E_PIPE、E_IO 或空間不足。 |
| `791c1fe21f2b7047555f834b` | user/libc.c:379 putchar | production | putchar 忽略單字元 write 失敗並回 c；應傳出失敗狀態。 |
| `d53a160b9abbd206b14adb6b` | user/sh.c:286 job_wait | production | SIGCONT 失敗仍把 j->stopped 清零並進入阻塞 wait；若群組尚停住可能卡住，應保留停止狀態並回報錯誤（ESRCH 可另作已退出處理）。 |
| `08f73500d01659c967f47933` | user/sh.c:386 run | production | 保存標準 fd 的 dup2 可能因原描述符無效失敗；未記錄成功與否就繼續修改 0/1 並在後面無條件 restore，可能破壞 shell I/O。sys_dup2 目前只回 EBADF，無配置失敗；仍需驗證 inherited fd 的前提。 |
| `85d0ecb1951d6ca6b0643ba5` | user/sh.c:387 run | production | 保存標準 fd 的 dup2 可能因原描述符無效失敗；未記錄成功與否就繼續修改 0/1 並在後面無條件 restore，可能破壞 shell I/O。sys_dup2 目前只回 EBADF，無配置失敗；仍需驗證 inherited fd 的前提。 |
| `c53e50832e15c9dc1386e1a0` | user/sh.c:391 run | production | 重新導向 dup2 的結果直接決定子命令實際輸入／輸出；失敗仍以 ok=1 spawn，可能讀錯來源或把資料寫到主控台。應中止命令並復原已保存 fd。 |
| `cb995cbb630effe7ceb75ef4` | user/sh.c:400 run | production | 重新導向 dup2 的結果直接決定子命令實際輸入／輸出；失敗仍以 ok=1 spawn，可能讀錯來源或把資料寫到主控台。應中止命令並復原已保存 fd。 |
| `1ded442cce717293b057ce71` | user/sh.c:405 run | production | 重新導向 dup2 的結果直接決定子命令實際輸入／輸出；失敗仍以 ok=1 spawn，可能讀錯來源或把資料寫到主控台。應中止命令並復原已保存 fd。 |
| `1275937945ea356535ccb425` | user/sh.c:411 run | production | 復原標準描述符的 dup2 失敗後仍當作正常流程；shell 後續讀寫會保留錯誤重新導向，須檢查並清理／報錯。 |
| `ad7cc269ccdf9b62c7e756cc` | user/sh.c:415 run | production | 重新導向 dup2 的結果直接決定子命令實際輸入／輸出；失敗仍以 ok=1 spawn，可能讀錯來源或把資料寫到主控台。應中止命令並復原已保存 fd。 |
| `cb4150fbe11d3f5372d92271` | user/sh.c:423 run | production | 復原標準描述符的 dup2 失敗後仍當作正常流程；shell 後續讀寫會保留錯誤重新導向，須檢查並清理／報錯。 |
| `b5aaebc1a787b9974dec9281` | user/sh.c:424 run | production | 復原標準描述符的 dup2 失敗後仍當作正常流程；shell 後續讀寫會保留錯誤重新導向，須檢查並清理／報錯。 |
| `320160ebbc803ade8bed771b` | user/top.c:153 sample | production | sysinfo 失敗仍把未更新／部分更新的資料當本次取樣，計算時間／CPU 使用率；應丟棄該次樣本或啟動失敗，保留有效基準。 |
| `57b6791eb86a44610724520b` | user/top.c:333 draw | production | 整個畫面 write 失敗或短寫被忽略；batch 模式可能成功退出但輸出不完整，互動模式持續產生無法送出的畫面。應處理 E_PIPE/短寫並復原主控台。 |
| `93aa45f92cd62bd3eee78eb5` | user/top.c:384 main | production | sysinfo 失敗仍把未更新／部分更新的資料當本次取樣，計算時間／CPU 使用率；應丟棄該次樣本或啟動失敗，保留有效基準。 |
| `7ac4c262e56db5f6f605b379` | user/ulib.h:41 puts_ | production | 通用輸出 helper 把負值／短寫丟掉（line_out 還把 len 清零），上層不能得知資料遺失；需讓 helper 提供狀態並由呼叫者決定錯誤策略，不能僅因 void API 就 baseline。 |
| `32c66539573e603cb6a11493` | user/ulib.h:74 line_out | production | 通用輸出 helper 把負值／短寫丟掉（line_out 還把 len 清零），上層不能得知資料遺失；需讓 helper 提供狀態並由呼叫者決定錯誤策略，不能僅因 void API 就 baseline。 |

## benign

| ID | 位置／函式 | 影響 | 判讀 |
| --- | --- | --- | --- |
| `7d269ddcdd845979eeaf9987` | user/cat.c:30 main | production | fd 已成功 open，讀完後只關閉自己的唯讀描述符；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `3788b065657fd39af376b1df` | user/cp.c:39 main | production | out 已成功 open 且寫入已檢查；關閉一次沒有 fd 競爭。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 若要保證持久化需另處理 sync/核心錯誤傳遞。 |
| `96384ea03b0bbd3f1346081c` | user/cp.c:40 main | production | in 已成功 open，複製期間沒有其他執行緒移除它；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `bc1d0e46810fb5f37e3b3af8` | user/init.c:202 mode_proc | selftest | 重新導向測試隨即斷言下一次 open 必須回 fd1；若 close 未生效、fd1 仍占用，測試會以 210/211 失敗，不會宣稱通過。 |
| `392b2f6fe7bfe81cbc508d11` | user/init.c:208 mode_proc | selftest | 重新導向測試隨即斷言下一次 open 必須回 fd1；若 close 未生效、fd1 仍占用，測試會以 210/211 失敗，不會宣稱通過。 |
| `b179bdd8279bedf08e7df5ca` | user/init.c:211 mode_proc | selftest | saved 是額外 console 參考，只作清理；主控台已由 open 回 fd1 的斷言復原。saved 開啟失敗亦不影響被測的重新導向結果。 |
| `e7289ca466d7601b6d19333d` | user/init.c:217 mode_proc | selftest | 讀取後的 out.txt 清理；任何開啟／讀取失敗會使後續 captured output 的精確內容斷言失敗。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `b62897867da159f38188e66d` | user/init.c:253 mode_proc | selftest | 先前已驗證寫入回 -E_TXTBSY 的有效 fd，只作釋放；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `77a00394e06df8176398de61` | user/init.c:345 mode_fork | selftest | exec 成功不返回；返回後子行程以 99/1 退出，父行程要求目標程式的既定退出狀態／訊號狀態，失敗不會假通過。 |
| `7f4f4bf4df8706ab90fb11ef` | user/init.c:684 mode_net | selftest | s2 已在前一個條件成功關閉後重新 udp_open(6000)，這裡釋放重新取得的有效 socket；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `fd54f6002a6095ce537bbea6` | user/init.c:689 mode_net | selftest | motd fd 與成功讀取已在前置條件驗證，唯讀清理；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `2eb13b2a87e04e78f5e8b426` | user/init.c:716 mode_net | selftest | 剛檢查 udp_open(5000)>=0，這裡關閉自己的有效 socket；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `9bf8d606047be81c2c0888bc` | user/init.c:726 mode_net | selftest | many[0..got) 與 s 都由成功 udp_open 取得；清理失敗僅可能無效 fd，無別的執行緒關閉它們，行程結束仍會回收。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `f5af384de3bb8b2c9dc0ef45` | user/init.c:727 mode_net | selftest | many[0..got) 與 s 都由成功 udp_open 取得；清理失敗僅可能無效 fd，無別的執行緒關閉它們，行程結束仍會回收。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `7980597495ed40af48320e7b` | user/init.c:941 perm_user_checks | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `2e51584ac174708f6ee25050` | user/init.c:970 perm_user_checks | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `23d99a7711c56eacc6f25bb3` | user/init.c:1023 perm_on | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `46374e740ba0d54c6f2d7613` | user/init.c:1027 perm_on | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `f7cc50397e5a29170157a6f7` | user/init.c:1034 perm_on | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `90858fca5e1a004f52e2fd69` | user/init.c:1098 perm_process | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `ab975ab82191e6788c569693` | user/init.c:1189 mode_time | selftest | 對應的 open／必要 read-write 或屬性設定已由緊鄰條件檢查；此為持有 fd 的單次清理，非測試成功的唯一依據。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `207fa1b8d7bafa14c4134fa7` | user/init.c:1270 mode_sighandler | selftest | 訊號壓力測試允許合併訊號；父行程最後要求至少5次交付與正確巢狀／運算結果，整批送不成功不會通過。子行程有限20次迴圈後退出。 |
| `0bd096281ec4f2ff2c9537db` | user/init.c:1292 mode_sighandler | selftest | me 是存活父行程；父端明確核對訊號計數或 read==-E_INTR，沒有成功交付會使該段測試失敗，送訊號的子行程亦會有限步驟退出。 |
| `6c1a7492ee633dbdd692412e` | user/init.c:1310 mode_sighandler | selftest | me 是存活父行程；父端明確核對訊號計數或 read==-E_INTR，沒有成功交付會使該段測試失敗，送訊號的子行程亦會有限步驟退出。 |
| `893487586a71dd7d0072f79c` | user/init.c:1312 mode_sighandler | selftest | 父端要求下一次 read 恰為2字元且首字元o；子端單次 write 的失敗／短寫會在父端精確長度斷言暴露，子行程接著退出。 |
| `182ebbf121e26fddde225e61` | user/init.c:1321 mode_sighandler | selftest | fds 由已檢查的 pipe 成功取得，子行程已等完；只關閉父端自己的兩個參考。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `b719b4e40e178a2948ac01a4` | user/init.c:1322 mode_sighandler | selftest | fds 由已檢查的 pipe 成功取得，子行程已等完；只關閉父端自己的兩個參考。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `37164fb0fcf98d7d7990c969` | user/init.c:1327 mode_sighandler | selftest | 使用合法固定訊號及已映射的 handler/restorer（或 SIG_DFL/IGN）；當前 sys_signal 不配置資源。後續測試亦核對 handler／忽略訊號的效果，非通用動態 handler 例外。 |
| `e48c01677b327c2c0f01329b` | user/init.c:1328 mode_sighandler | selftest | fds[1] 已關閉，零長度 write 在此最多回 -E_BADF；沒有資料傳輸或同步完成承諾，刪除這個無效 checkpoint 亦可。不要當成已通知父行程的證據。 |
| `0ce597f743fb689d14088bd7` | user/init.c:1342 mode_sighandler | selftest | 使用合法固定訊號及已映射的 handler/restorer（或 SIG_DFL/IGN）；當前 sys_signal 不配置資源。後續測試亦核對 handler／忽略訊號的效果，非通用動態 handler 例外。 |
| `8b8aace0a1aa4d6917d11f48` | user/init.c:1352 mode_sighandler | selftest | 使用合法固定訊號及已映射的 handler/restorer（或 SIG_DFL/IGN）；當前 sys_signal 不配置資源。後續測試亦核對 handler／忽略訊號的效果，非通用動態 handler 例外。 |
| `e36c2018f9cfaeb27955325f` | user/init.c:1358 mode_sighandler | selftest | exec 成功不返回；返回後子行程以 99/1 退出，父行程要求目標程式的既定退出狀態／訊號狀態，失敗不會假通過。 |
| `f6a4af93ac7e27ecf9ff4335` | user/init.c:1389 mode_sighandler | selftest | 使用合法固定訊號及已映射的 handler/restorer（或 SIG_DFL/IGN）；當前 sys_signal 不配置資源。後續測試亦核對 handler／忽略訊號的效果，非通用動態 handler 例外。 |
| `ffc727c8de2f908893d71a14` | user/init.c:1390 mode_sighandler | selftest | 零長度 stdout write 只作進出 syscall 的 checkpoint，不傳輸資料；之後的故意壞 stack 與退出狀態斷言決定結果。 |
| `76bf9fc17310f32ce37f914e` | user/init.c:1567 mode_heap | selftest | double-free 子行程故意關閉 stderr 以抑制診斷；即使已經關閉也不影響父端驗證退出碼134，屬盡力清理。 |
| `dc058157370fd909915c5feb` | user/init.c:1680 fileops_on | selftest | close 後緊接以同一 fd 操作要求 -E_BADF；close 未生效就會使斷言失敗。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `2b30fe0092b5aa76e762eada` | user/init.c:1691 fileops_on | selftest | 前面的 open 即使失敗，接著 rename E_EXIST 與 stat(a) 成功的斷言也會偵測 setup 不成立；close 只是釋放成功時取得的 fd。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `21755e4af24f896088157f48` | user/init.c:1726 fileops_on | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `011c82f53030dd5905bc1486` | user/init.c:1730 fileops_on | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `fa8d029bba10ac9640a2a60d` | user/init.c:1856 mode_disk | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `5f0cba972b77357a55bb0087` | user/init.c:1857 mode_disk | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `61b07916804089337af955d6` | user/init.c:1952 mode_stop | selftest | 對自己的 SIGSTOP 效果由父端 proc_state==STOPPED 與 WNOHANG==0 驗證；未停止會失敗，不會假通過。 |
| `f05eb4f35d652694a70d260b` | user/init.c:1984 mode_stop | selftest | 父端要求 read 完整 sizeof(g) 且 g>0，再確認中間子行程退出與孤兒回收；此 write 失敗會以 EOF／長度斷言暴露。 |
| `21c070a80de6210a59e39d21` | user/init.c:1987 mode_stop | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `113f6979c1b808d9ad808c94` | user/init.c:1990 mode_stop | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `f05c03bca485f1269266106e` | user/init.c:2055 mode_jobs | selftest | a 在 fork_sleeper 後已驗證>0；父端以最多200次輪詢斷言 ZOMBIE，再 WNOHANG 要求 SIGKILL 狀態。送不成功會有限時間測試失敗。 |
| `07679d17ca6088aff2073966` | user/init.c:2076 mode_jobs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `46b4a1043f3006db00f8821d` | user/init.c:2100 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `076fba7dcbc28a8e019de4a2` | user/init.c:2101 mode_pipe | selftest | 父端最後要求完整字串 hello through a pipe 與固定退出碼21；任一 write 失敗／短寫都無法通過精確內容比對。 |
| `0856629bd55385569f24c519` | user/init.c:2102 mode_pipe | selftest | 父端最後要求完整字串 hello through a pipe 與固定退出碼21；任一 write 失敗／短寫都無法通過精確內容比對。 |
| `a999c8c87c3ecb4b90e33ab0` | user/init.c:2105 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `d2821b4f2d1dd43183340658` | user/init.c:2110 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `55c710508ca82742ed8f14a8` | user/init.c:2121 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `6f4bcabcbe47c216462b4601` | user/init.c:2130 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `e65dadfc8634ef6759d720de` | user/init.c:2138 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `601d7014de738e003f47cf4b` | user/init.c:2147 mode_pipe | selftest | pipe fd 有效且目的fd1在界內，當前 dup2 不配置資源；失敗亦會由父端 captured by a pipe 精確內容斷言暴露。保存／復原那兩處另列 real，不能一併接受。 |
| `666ad1ce622cfc17aea5923b` | user/init.c:2148 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `19d8459083bb5950c03a88ce` | user/init.c:2152 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `ded9342ed99776bd7adda667` | user/init.c:2157 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `1a6537136240a8dec6109a03` | user/init.c:2164 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `4cbc756f41a3b1896a9a8080` | user/init.c:2167 mode_pipe | selftest | 端點來自已檢查的 pipe；關閉自己的讀／寫參考，不涉及配置或跨執行緒 close。後續 EOF、內容長度、-E_PIPE 或退出狀態斷言仍驗證測試結果。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `3300e67d3f6b5d056904ff00` | user/init.c:2208 mode_fs | selftest | close 後緊接以同一 fd 操作要求 -E_BADF；close 未生效就會使斷言失敗。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `8e624fe355b3f923f9d66316` | user/init.c:2222 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `d32ed0e4ca80eac50624f587` | user/init.c:2233 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `0e255bcc5f2e646124f75311` | user/init.c:2237 mode_fs | selftest | 附加測試最後讀回並精確要求 one two；任何寫失敗／短寫會令測試失敗，不把資料遺失當成功。 |
| `6d83588b0f99b11f0736932d` | user/init.c:2238 mode_fs | selftest | 附加測試最後讀回並精確要求 one two；任何寫失敗／短寫會令測試失敗，不把資料遺失當成功。 |
| `dac587586d1cb521b4cc7c78` | user/init.c:2239 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `ed33e12c89f3117f427b56be` | user/init.c:2242 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `14a308f4992bb2e597617c3c` | user/init.c:2260 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `5651a931788282734e1fe4d5` | user/init.c:2276 mode_fs | selftest | 對應 open/read-write 的成功／具體內容或型別已由鄰接測試驗證；這裡僅釋放自己的 fd，沒有額外 close 失敗可回報（除 EBADF）。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `9064439bd46e622ec9be4cbe` | user/ls.c:50 main | production | 成功開啟的目錄唯讀 fd 在列舉後關閉；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `8e706809b832fdc738e0c29b` | user/passwd.h:26 passwd_find | production | fd>=0 才 close，僅查詢唯讀 passwd；讀取負值／空檔已由 n<=0 處理，close 不提供額外讀取錯誤資訊。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `f2d9552b48adcaa7be21445a` | user/sh.c:392 run | production | 關閉成功 pipe/open 得到且只由 shell 持有的臨時 fd；dup2 是否成功須在相鄰 real 項處理，清理本身僅可能 EBADF，不可把此例外轉套到 dup2。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `170505da422b85198ce84874` | user/sh.c:401 run | production | 關閉成功 pipe/open 得到且只由 shell 持有的臨時 fd；dup2 是否成功須在相鄰 real 項處理，清理本身僅可能 EBADF，不可把此例外轉套到 dup2。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `def6b275abf6cb69d360a4de` | user/sh.c:406 run | production | 關閉成功 pipe/open 得到且只由 shell 持有的臨時 fd；dup2 是否成功須在相鄰 real 項處理，清理本身僅可能 EBADF，不可把此例外轉套到 dup2。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `91c115f5193737cf646c09b9` | user/sh.c:416 run | production | 關閉成功 pipe/open 得到且只由 shell 持有的臨時 fd；dup2 是否成功須在相鄰 real 項處理，清理本身僅可能 EBADF，不可把此例外轉套到 dup2。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `9aec062889271fc915f71dc9` | user/sh.c:425 run | production | 關閉 shell 保留的 SAVE_IN/SAVE_OUT 臨時槽位；若前面的保存失敗，本行至多再回 EBADF。原始保存／復原錯誤已分別列 real；此行是清理，不是保存成功的證據。 |
| `7a1c3caa70c80707852eb671` | user/sh.c:426 run | production | 關閉 shell 保留的 SAVE_IN/SAVE_OUT 臨時槽位；若前面的保存失敗，本行至多再回 EBADF。原始保存／復原錯誤已分別列 real；此行是清理，不是保存成功的證據。 |
| `51ae7c314b756d146c0f2bad` | user/sh.c:435 run | production | prev_rd>=0 才關閉 shell 未使用的管線讀端；不要求回傳資料給呼叫者。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `324364fb831bf4cf9975fc3f` | user/su.c:36 main | production | exec 成功不返回；任何返回路徑隨即印 cannot run/start 並回 127，已有明確失敗處理，不需要再判斷負值。 |
| `12cb017005d27cc275fc9265` | user/su.c:40 main | production | exec 成功不返回；任何返回路徑隨即印 cannot run/start 並回 127，已有明確失敗處理，不需要再判斷負值。 |
| `dd35cfd415e68344614bfd3f` | user/tftp.c:81 main | production | 已有伺服器／逾時／寫入錯誤訊息且即將 return 1；關閉已開啟 out 為清理，不能再把失敗操作變成功。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `74a5f793c08680184266b3c8` | user/tftp.c:92 main | production | 已有伺服器／逾時／寫入錯誤訊息且即將 return 1；關閉已開啟 out 為清理，不能再把失敗操作變成功。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `891eaa27e7d494d97b056b28` | user/tftp.c:99 main | production | 已有伺服器／逾時／寫入錯誤訊息且即將 return 1；關閉已開啟 out 為清理，不能再把失敗操作變成功。目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `6674f65aa4c4d686dcdbe399` | user/tftp.c:110 main | production | 成功下載後關閉已開啟 out，沒有其他執行緒更動 fd；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 但下載完成的持久化保證需要獨立的 sync／flush 錯誤介面。 |
| `fb6c6c158c099a8185943a9f` | user/tftp.c:111 main | production | socket 從 udp_open 成功取得且只關一次；目前 sys_close 只會因 fd 無效回 -E_BADF；vfs_close/closed 是 void，不能用檢查 close 偵測磁碟 flush 錯誤。 |
| `142f09d66212d5330ca2d3d3` | user/top.c:372 on_terminate | production | 即將退出／訊號終止前的顏色與游標復原是盡力清理；寫不出去也必須繼續還原 console mode 並 exit，沒有可恢復的輸出通道。 |
| `84b0431ec710fe1b7dc78ef5` | user/top.c:398 main | production | SIGINT/SIGTERM 是合法可攔截訊號，on_terminate 與 usigrestorer 在目前已映射程式碼；sys_signal 不配置資源，只在無效訊號／映射時失敗。例外限目前固定 handler 與未被改動的地址空間。 |
| `0b873304ba561ef7dedb053b` | user/top.c:399 main | production | SIGINT/SIGTERM 是合法可攔截訊號，on_terminate 與 usigrestorer 在目前已映射程式碼；sys_signal 不配置資源，只在無效訊號／映射時失敗。例外限目前固定 handler 與未被改動的地址空間。 |
| `466bf61d0307f13b6858d0fc` | user/top.c:400 main | production | 清畫面的 ANSI 控制序列只影響初始外觀；後續 draw 會重畫，draw 的實際資料寫入另列 real，不能由此接受所有 stdout write。 |
| `c922c17129324e03f2049732` | user/top.c:442 main | production | 即將退出／訊號終止前的顏色與游標復原是盡力清理；寫不出去也必須繼續還原 console mode 並 exit，沒有可恢復的輸出通道。 |

