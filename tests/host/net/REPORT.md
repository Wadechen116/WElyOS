# 網路堆疊驗證紀錄

2026-11-18，Codex。狀態：協定堆疊可供整合審閱；DNS 與最終零警告驗收尚待協作回覆，**不是完整交付**。

## 已實作

- `kernel/net/stack.c`：Ethernet、ARP、IPv4、ICMP echo、UDP；單介面、無動態配置、外部序列化。
- `kernel/net/wire.h`：逐位元組的網路序讀寫與檢查碼。
- `tests/host/net/test_net.c`：假網卡、精確長度的接收緩衝區、可重現的損壞輸入。
- `scripts/host-test-net.sh`：ASan/UBSan 與三架構物件編譯；輸出在 `/tmp/welyos-net.*`，不碰 `build/`。

ARP 快取、等待表、UDP 綁定各八格。等待表依下一跳索引，不覆蓋已接受的封包。
UDP 接受 IPv4 的零檢查碼，送出一律計算，零結果編成 `0xffff`。
不支援本機 loopback、廣播/群播 UDP、IPv4 選項或分段；不生成 ICMP unreachable。
ping 回呼通知有效 echo reply，呼叫端仍須比對來源、id、seq，實作逾時；回呼不是請求身分驗證。

## 本輪實測

| 檢查 | 結果 |
|---|---|
| ASan + UBSan | PASS，1,064,208 項 CHECK，無報告 |
| 所有合法 ARP/ICMP/UDP 範本的截斷點 | PASS |
| IPv4 total length、UDP length 的全部 65,536 個值 | PASS |
| ARP 位址長度、IPv4 version/IHL 的全部 256 個值 | PASS |
| 固定種子隨機封包 | 100,000 原始 + 100,000 結構化損壞，PASS |
| ARP 解析、匹配、抗覆寫、資料複製、容量、重試與放棄 | PASS |
| 快取 60 秒過期與 uint32 時間回繞 | PASS |
| ICMP/UDP 最大 MTU、奇數資料、UDP 零結果檢查碼 | PASS |
| 假驅動傳送失敗、四類 drop 總和不變量 | PASS |
| i386 / riscv64 / aarch64 freestanding `-O2` 物件編譯 | PASS（下述共享標頭警告例外） |
| `nm -u` 物件依賴 | 僅 memcmp/memcpy/memset；i386 另有 GOT；無 libgcc 算術依賴 |

本次開發驗證暫時加 `-Wno-error=comment`，因為 Claude 所有的 `include/net.h` 第 14、78 行註解含有 slash-star，觸發 `-Wcomment`。
警告仍有顯示，並未當成零警告。正式腳本保留 `-Wall -Wextra -Werror`，目前會因此失敗；已透過 memory 的 086、087 請 Claude 修正。

本輪 sanitizer 命令：

```sh
gcc -std=c11 -Wall -Wextra -Werror -Wno-error=comment -fno-builtin \
    -iquote include -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-pie -no-pie tests/host/net/test_net.c kernel/net/stack.c \
    -o /tmp/welyos-test-net-dev
/tmp/welyos-test-net-dev
```

共享標頭修好後，正式入口：`bash scripts/host-test-net.sh`。

## 尚未驗證／外部相依

- DNS 函式簽名已於 memory 086 提案；依 `docs/net-design.md`，待雙方同意後才實作 `user/dns.h` 與 DNS 測試。
- 共享標頭修正後需重跑正式腳本，不能用開發命令取代零警告驗收。
- QEMU、virtio-net、syscall、實際網路與 DNS 伺服器由 Claude 整合驗證；本輪完全未執行。
- 無 WCET、形式證明、真實硬體或抗輻射驗證；沒有提交或推送。

封包格式參考：
[IPv4 RFC 791](https://www.rfc-editor.org/rfc/rfc791)、
[UDP RFC 768](https://www.rfc-editor.org/rfc/rfc768)、
[ARP RFC 826](https://www.rfc-editor.org/rfc/rfc826)。
