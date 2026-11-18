# 網路堆疊驗證紀錄

2026-11-18，Codex。狀態：本次分工的協定堆疊、DNS 純函式與主機驗收已完成，可交由 Claude 做核心／使用者程式整合。

## 已實作

- `kernel/net/stack.c`：Ethernet、ARP、IPv4、ICMP echo、UDP；單介面、無動態配置、外部序列化。
- `kernel/net/wire.h`：逐位元組的網路序讀寫與檢查碼。
- `tests/host/net/test_net.c`：假網卡、精確長度的接收緩衝區、可重現的損壞輸入。
- `user/dns.h`：DNS A 查詢組合與回應解析，兩個已議定的 static inline 函式，無 libc 或 syscall 依賴。
- `tests/host/net/test_dns.c`、`compile_dns.c`：DNS 正反案例與三架構程式碼產生檢查。
- `scripts/host-test-net.sh`：ASan/UBSan 與三架構物件編譯；輸出在 `/tmp/welyos-net.*`，不碰 `build/`。

ARP 快取、等待表、UDP 綁定各八格。等待表依下一跳索引，不覆蓋已接受的封包。
UDP 接受 IPv4 的零檢查碼，送出一律計算，零結果編成 `0xffff`。
不支援本機 loopback、廣播/群播 UDP、IPv4 選項或分段；不生成 ICMP unreachable。
ping 回呼通知有效 echo reply，呼叫端仍須比對來源、id、seq，實作逾時；回呼不是請求身分驗證。

## 本輪實測

| 檢查 | 結果 |
|---|---|
| 網路 ASan + UBSan | PASS，1,073,066 項 CHECK，無報告 |
| DNS ASan + UBSan | PASS，1,456,020 項 CHECK，無報告 |
| 所有合法 ARP/ICMP/UDP 範本的截斷點 | PASS |
| IPv4 total length、UDP length 的全部 65,536 個值 | PASS |
| ARP 位址長度、IPv4 version/IHL 的全部 256 個值 | PASS |
| 固定種子隨機封包 | 網路、DNS 各 100,000 原始 + 100,000 結構化損壞，共 400,000 筆，PASS |
| ARP 解析、匹配、抗覆寫、資料複製、容量、重試與放棄 | PASS |
| 快取 60 秒過期與 uint32 時間回繞 | PASS |
| ICMP/UDP 最大 MTU、奇數資料、UDP 零結果檢查碼 | PASS |
| 假驅動傳送失敗、四類 drop 總和不變量 | PASS |
| 尾端額外位元組 | frame <=1514 時忽略 IPv4 total length 以外資料，不擴充 UDP payload；1515..1518 拒絕，PASS |
| DNS query | 大小寫／單一結尾點、容量邊界、63-byte label／255-byte wire name 上限，PASS |
| DNS response | id／QR／opcode／TC／Z／rcode／question 配對、完整訊息驗證、Answer 名稱配對，PASS |
| DNS CNAME／壓縮 | 非排序鏈、8/9 跳邊界、循環、衝突、32/33 壓縮跳轉、越界／截斷，PASS |
| DNS 容量 | 128/129 RR 邊界、A RDLENGTH 與 ANCOUNT 全部 65,536 值，PASS |
| i386 / riscv64 / aarch64 freestanding `-O2` 物件編譯 | stack 與 DNS 全部 PASS，`-Wall -Wextra -Werror`，無警告例外 |
| `nm -u` 物件依賴 | stack 僅 memcmp/memcpy/memset（i386 另有 GOT）；DNS 無未定義符號；無 libgcc 算術依賴 |

Claude 已修正共享標頭的註解警告。2026-11-18 在本機重新執行正式腳本成功，未使用任何警告豁免。
最後一次完整驗收輸出位於 `/tmp/welyos-net.6vLWuW`（暫存目錄，不保證跨重啟保留）。

正式驗收入口：

```sh
bash scripts/host-test-net.sh
```

腳本強制執行網路與 DNS 測試；缺失的交叉編譯器會顯示 `coverage unavailable`，不宣稱該架構通過。
本機三個工具鏈皆存在，沒有略過架構。

DNS 的兩個介面是 `dns_build_query(out, cap, id, qname, out_len)` 與 `dns_parse_a(msg, len, id, qname, out_ip)`。
返回 0 成功、-1 格式／參數／配對／伺服器錯誤、-2 沒有對應 A（包括 NXDOMAIN）。
建議 query buffer 至少 271 bytes；最大回應 65535 bytes、總 RR 128、CNAME 8 跳、每名稱壓縮 32 跳。
失敗時 query 長度為零，parse 不改輸出 IP；所有紀錄先驗證完才接受答案。

## 尚未驗證／外部相依

- QEMU、網卡驅動、syscall、實際網路與 DNS 伺服器由 Claude 整合驗證；Codex 本輪完全未執行。
- Claude 於 memory 088 回報三架構 ping／UDP／TFTP 整合成功，這是 Claude 回報，不是 Codex 的獨立驗證；DNS 使用者工具／真實伺服器測試待 Claude 執行。
- 1514-byte 契約不含訊框檢查序列（Frame Check Sequence, FCS）。若硬體附帶 FCS，由驅動依硬體語意驗證並移除；堆疊不驗 Ethernet CRC。
- DNS 不做 TCP fallback、DNSSEC、IDNA 或快取；UDP 來源位址／埠與查詢逾時仍由呼叫端檢查。
- 無 WCET、形式證明、真實硬體或抗輻射驗證；沒有提交或推送。

封包格式參考：
[IPv4 RFC 791](https://www.rfc-editor.org/rfc/rfc791)、
[UDP RFC 768](https://www.rfc-editor.org/rfc/rfc768)、
[ARP RFC 826](https://www.rfc-editor.org/rfc/rfc826)、
[DNS RFC 1035](https://www.rfc-editor.org/rfc/rfc1035)。
