# WElyOS 網路:分工與介面

這份文件是 Claude 與 Codex 分工的依據。介面以 `include/net.h` 為準(已凍結:之後只能新增,不改名、不移除)。

## 範圍(第一階段)

乙太網路(Ethernet)、ARP(Address Resolution Protocol)、IPv4、ICMP(Internet Control Message Protocol)的 echo、UDP(User Datagram Protocol)。
不做:TCP、IP 分段與重組、IPv6、DHCP(位址固定)、路由表(只有一個預設閘道)。

QEMU 的使用者模式網路(slirp)提供的環境:本機 `10.0.2.15/24`,閘道 `10.0.2.2`(會回應 ping),DNS `10.0.2.3`(UDP 53)。

## 誰負責什麼

| 部分 | 負責 | 檔案 |
|---|---|---|
| 介面 | Claude | `include/net.h`、本文件 |
| 協定堆疊 | **Codex** | `kernel/net/*.c`、`kernel/net/*.h`(內部標頭) |
| 主機端測試 | **Codex** | `tests/host/net/*`、`scripts/host-test-net.sh` |
| DNS 訊息的組合與解析(純函式,給使用者程式用) | **Codex** | `user/dns.h`、其測試放在 `tests/host/net/` |
| 網路卡驅動程式、中斷、收送執行緒 | Claude | `kernel/virtio_net.c`、`arch/*` |
| 上鎖、系統呼叫、把堆疊接進 kernel | Claude | `kernel/netif.c`、`kernel/proc.c` |
| 使用者程式(`ping` 等) | Claude | `user/*.c` |
| 建置設定、QEMU 執行、回歸測試、DEVLOG / README | Claude | `CMakeLists.txt`、`scripts/*`、`docs/*` |
| 使用者與權限、記憶體不足的處理、`mmap` | Claude | kernel 各處 |

只有負責的一方修改自己的檔案;需要對方改,就在 memory server 留訊息。

## 對協定堆疊的要求

`include/net.h` 開頭的六條規則之外:

1. **C11、freestanding**,以三個架構的編譯選項(`-Wall -Wextra`,不可有警告)與主機的 gcc 都能編譯。
   可引用的標頭只有 `<stddef.h>`、`<stdint.h>`、`net.h`、`kstring.h`、`fs.h`(只為了 `E_*`)。
   主機測試自己提供 `kstring.h` 需要的函式(直接用 libc 的即可)。
2. **目標架構都是 little-endian,但程式不可以依賴這一點**:讀寫封包欄位一律逐位元組存取,不把指標轉型成結構來讀。
3. **註解用繁體中文**,說明「為什麼」;專業名詞第一次出現時括號附英文全名(專案慣例)。
4. **安全優先**:收到的每個長度與位移先檢查再使用;IPv4 標頭長度、總長度、ICMP 與 UDP 的長度都要和實際收到的長度互相核對;檢查碼錯誤的丟掉。
   不回應廣播的 ping(避免被用來放大攻擊);ARP 只學習「回答我們問過的」與「問我們的」,不接受來路不明的紀錄覆蓋閘道的 MAC。
5. **ARP**:快取至少 8 筆,紀錄有期限(60 秒);詢問在第 0、1、2、3 秒各送一次,第 4 秒放棄並丟掉留著的封包。等待中的封包以下一站為單位各留一個,已有封包在等時再送回傳 `-E_BUSY`(細節見 `net.h`)。
6. **主機端測試**(`bash scripts/host-test-net.sh`,不需要 QEMU):以一個假的 `netif`(`send` 把訊框記下來)驅動堆疊,至少涵蓋
   - ARP 詢問與回答的完整流程、快取過期、重問與放棄
   - 回答別人的 ping;自己 ping 出去並收到回答;檢查碼
   - UDP 收送、綁定衝突、沒有人綁定的埠
   - 每一種截斷或欄位不合理的封包(逐一把合法封包的每個長度欄位改壞、把訊框從每個位置截斷):不可以當掉、不可以越界(以 `-fsanitize=address,undefined` 執行)
   - 一段隨機位元組的訊框(固定種子,至少十萬個)
7. 不執行 QEMU、不動 `build/`(那是 Claude 的;主機測試自己用別的目錄)、不 commit。

## 整合時的呼叫方式(Claude 這一側,供參考)

```
接收執行緒:  從驅動程式取出訊框 -> 上鎖 -> net_rx(nif, frame, len) -> 解鎖
計時:        每 100ms -> 上鎖 -> net_tick(now_ms) -> 解鎖
系統呼叫:    上鎖 -> net_ping_send / net_udp_send / net_udp_bind -> 解鎖
回呼:        在鎖內被呼叫;只把資料複製到等待者的緩衝區並叫醒它,不呼叫 net_*
```

## 可驗證的性質(與航太飛行軟體的做法對照)

Codex 審閱介面後提出、雙方同意納入的性質。每一條都要有對應的主機端測試:

| 性質 | 做法 |
|---|---|
| 容量固定 | 所有表格是靜態陣列;沒有動態配置 |
| 重試與佇列有上限 | ARP 最多問四次;每個下一站只留一個等待中的封包;表格滿了明確拒絕 |
| 狀態轉移與時間界線明確 | ARP 紀錄的每個狀態(詢問中、有效、過期)與轉移條件寫在程式開頭;時間一律以模 2^32 的差計算 |
| 嚴格驗證輸入 | 長度、位移、檢查碼逐項核對;IPv4 選項與分段一律拒絕 |
| 故障分類統計 | `struct net_stats` 把丟棄的原因分成四類,每個訊框只記一類 |
| 可重現的錯誤注入 | 主機測試以固定種子產生截斷、欄位損壞與亂數的訊框,搭配 sanitizer |

界線:主機與 QEMU 上的結果不是最壞執行時間(WCET)的證明,也不代表抗輻射或真實硬體上的容錯能力。

## DNS(`user/dns.h`)

純函式,呼叫者提供緩衝區與容量。解析回應時必須核對:查詢編號、是回應(QR)、opcode、rcode、問題區段與原本問的名稱相同;
名稱壓縮的指標跳轉有上限並拒絕循環;CNAME 鏈有上限,而且只接受「對應到查詢名稱」的 A 紀錄,不可以拿回應裡第一個出現的 A 紀錄。
函式簽名由 Codex 先在 memory server 提出,雙方同意後再實作與整合。
