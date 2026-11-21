# c32-tempobj 固定里程碑只讀證據核對（2026-11-21）

核對 `build/milestones/c32-tempobj/manifest.txt`、實際boot檔、三個既有QEMU log及 `build/c32a/regress-t4.txt`。本輪沒有啟動QEMU、重建boot、執行host測試、改產品或commit。以下「log顯示成功」不是我方獨立重跑QEMU。

## 完整性

四個boot檔完整SHA-256全部符合manifest，包括AArch64實際boot所需.bin。三個log完整hash的前16碼符合manifest，regress亦符合記錄前綴，尾行為「全部符合預期」。manifest目前只給log/regress短hash，本報告補記讀到的完整值，不把短前綴說成原manifest已保存完整hash。

| boot | 完整SHA-256 |
| --- | --- |
| aarch64-welyos.bin | 43ea4b64571a223dd6006e265997bc909378ffd42cb788beeea6f689a74e5b63 |
| aarch64-welyos.elf | fdae65ff6bee5d05479bbeeaf30583abd3bf500cb95ed5a9f2a4e5fd065e0e8e |
| i386-welyos.elf | 3699e49f904ca91efc47fdd6f310e6d01af79520a2748407b27f0953d191594c |
| riscv64-welyos.elf | 5b8dc2ba71896b7b2fa465b66dabc7eea4a94947c756e091536c5cc0df7dd6ab |

| 既有證據檔 | 完整SHA-256 |
| --- | --- |
| qemu-i386-selftest.log | 75167f47671b86d19b4b37cc37f8d2133f415c489aa3c9c208f370a8243ac824 |
| qemu-riscv64-selftest.log | 99bb480b4c1ce20bc7dac3b47424e8e9d6dfde8080609bad97df4d6056ded6d2 |
| qemu-aarch64-selftest.log | 61d83671a2593b120d6c2ae442970c745fd43cffd7aab088bbccf3c719807b35 |
| build/c32a/regress-t4.txt | 44d6e1bfd74aedfa448775cbd9a966fd0311302241d81d04d9f55c4665b347c7 |
| manifest.txt | c74d52570b912b962467f7b3fd7a4d6c436ab77e59e29ec941eb099cf7235abf |

8個歷史來源hash中，目前6個符合；proc.c與proc_internal.h因owner已接著做c1而不同。固定包沒有保存這兩個原始碼副本，因此不把c1當歷史來源，也不把合理的working source漂移誤報成固定boot損壞。固定boot、log與regress本輪完整性核對都通過。

## 行為證據與精確assert

三個固定log均有：thread KOBJ 6 pages；creator退出後pipe/file仍持4 pages且最後close退回；RAMFS首配置拒絕回-12、size stays0；全部user processes exited, memory reclaimed；20 failed creations **20 failed after stack mapped**，slots0x1→0x1、pages6→6；最後 `exit: code 0 to the emulator`。未見panic/assertion failed。manifest宣告各架構wrapper rc0與4CPU；我方只核對記錄，未重新觀測退出碼或工具程序。

`kernel/test/mem_test.c`目前hash仍符合歷史manifest，故可直接讀相同測試來源：pipe_kobj_test:989–996獨立以兩file及pipe/inode的kmalloc_size分別向上取頁求expect，斷言used[KOBJ]==expect、refs==4；最後close後used0、refs1。這已補掉先前只斷k>=3的盲點，不只是印出4。使用相同mb_pages_for_bytes仍有helper共同依賴，但不是從產品charge欄位回讀當expected。

`kernel/test/user_test.c`亦與manifest相符，斷言ok==0、reached==20、slot/page前後相等。對proc本體不借用已改c1來源冒稱歷史驗證，而額外只讀固定i386 ELF反組譯確認：

- proc_thread_failure_probe在0x11c66d讀目標proc+0x48作初值，結尾0x11c6ff–0x11c709讀同一目標欄位、相減寫到reached輸出；不是全域counter。每輪重新mb_stats，0x11c6a2加3設定limit，這是固定tempobj版本，不能與當前c1的+255公式混用。
- user_thread_prepare在0x11ae61呼叫thread_prepare；失敗分支於解除／放slot後0x11aeff `lock addl $1,0x48(%edi)`，%edi為該目標proc。入口ustart charge提前拒絕走0x11af0c，不會經此counter。故log的reached20有對應後段失敗到達證據，不再是舊「pages相等但從沒碰stack」情況。
- 固定i386 user_thread_main在0x11b507 kfree arg，0x11b517 mb_release_object，早於dying/killed判斷；sys_fork在0x11bdb0 charge tempobj，0x11be1a thread_prepare，0x11be32才fork_space，維持父COW提交前完成可失敗charge。
- fork_child_main於0x11b5b5將frame拷到kernel stack，0x11b5bc呼叫fork_start_release，之後才dying/killed；release於0x11a783/78b先free frame與start，再0x11a79c退聚合KOBJ/reference。此固定實作以一個fork_start擁有聚合frame+start成本與一份domain reference，不能硬套先前建議的「兩份domain reference」測試oracle。

這些為i386固定ELF靜態核對，不是三架構反組譯或故障注入。log沒有獨立tempobj每階段delta marker，正常proc/fork完成與memory reclaimed也不能代替各quota/ref失敗點驗收。root/其他agent的tempobj host evidence應另外綁其來源hash，不能自動回填成這個固定里程碑已做全故障測試。

regress-t4涵蓋記錄：三架構各26/26情境、磁碟16/16、安全模式5/5、1/4/8CPU各重複3次；net 1073066 checks，架構界線／編譯防線、靜態新增0需要處理提示、exit5-class5/5，最後全部符合預期。這是原owner既有regress記錄完整性核對，非本輪重新執行。

## 下一依賴

1. 若要宣布我方獨立三架構integration通過，需root另以這四個固定boot副本重跑QEMU並保留本次命令/log/退出碼；本報告不取代它。
2. 歷史source有hash卻無副本，待owner保存tempobj時點來源archive或commit+精確dirty patch，才能對全部tempobj配置失敗源碼重新抽取驗證；不能直接拿已變c1來源。
3. c1新stack承諾／退出回收須新里程碑、獨立fork總量與ticket公式審查。reached20只能結此tempobj後段到達範圍，不結整255頁預留、STACK COW/fork、image/exec、RAMFS分類配額或所有架構故障點。

## 附錄：最小可重現manifest規格

每個milestone應有同目錄不可變來源及boot/log副本；避免把可繼續覆寫的build/c32a/regress路徑當唯一來源。

- **來源**：完整base commit；dirty時保存 `git diff --binary` 的精確patch與未追蹤source清單/內容，或直接完整source tar。記每個來源檔完整SHA-256，另記patch/archive hash。只寫「base+uncommitted」不足以重建。
- **建置**：cwd、實際build命令、toolchain/flags/版本、產物名稱；generated headers/rootfs/initrd等實際輸入納入hash或archive。來源與建置參數對應同一時點，禁止在owner繼續改檔後重建假稱原版。
- **boot**：所有被emulator真正讀取的檔案，三ELF之外包含AArch64.bin、若使用磁碟映像/固件也列各完整hash；明示ELF→bin選擇規則與實際路徑。
- **執行**：完整emulator invocation或wrapper與版本/hash、cwd、CPU/memory、machine/firmware、guest args、disk/net隔離、timeout、開始/結束時間、wrapper rc與guest exit marker各自列明。
- **證據**：stdout/stderr/QEMU log、regress原檔以完整SHA-256保存；每case列必需marker（例如reached==rounds、pipe expect/ref結尾、RAMFS EOF與guest exit0），並檢查panic/assertion。正負對照分開，記來源變異patch hash及預期exit，不以wrapper0代替客體真正輸出。
- **不變性**：來源snapshot、boot、log取完hash後保留不可變副本。manifest也另存完整hash，重驗前後核對；後續working-tree source差異獨立報告，不污染固定boot結論。
