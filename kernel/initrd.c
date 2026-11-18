/*
 * initrd.c - 開機檔案包:把嵌在 kernel 裡的一包檔案解開到檔案系統
 *
 * 問題:檔案系統剛建立時是空的,而 kernel 還沒有磁碟驅動,第一批檔案從哪裡來?
 * 做法(Linux 的 initramfs 也是這樣):建置時把一個目錄打包成一個檔案,
 * 整包嵌進 kernel 映像;開機時把它解開到記憶體檔案系統。
 * 打包的來源是原始碼的 rootfs/ 目錄,加上編譯好的使用者程式(/bin/init)。
 *
 * 格式用 tar(ustar):主機上的 tar 指令就能產生,不需要自己寫打包工具。
 * tar 的結構很單純,一連串的「512 位元組的標頭 + 檔案內容(補到 512 的倍數)」,
 * 最後以全 0 的區塊結束。標頭裡的數字是八進位的文字。
 *
 *   位移   長度  內容
 *     0    100   名稱
 *   124     12   大小(八進位文字)
 *   156      1   類型:'0' 或 0 = 一般檔案,'5' = 目錄
 *   257      5   "ustar"
 *   345    155   名稱的前半段(名稱超過 100 個字元時使用)
 *
 * 這包資料來自建置過程,不是外部輸入;但解析時仍然檢查每個長度都在範圍內。
 */
#include "fs.h"
#include "kstring.h"

#define BLK 512u

static uint64_t octal(const uint8_t *p, size_t n)
{
    uint64_t v = 0;

    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

/* 把 src(最多 n 個字元,可能沒有結尾的 0)接到 dst 後面;放不下回傳 -1 */
static int append(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t len = strlen(dst);

    for (size_t i = 0; i < n && src[i]; i++) {
        if (len + 1 >= cap)
            return -1;
        dst[len++] = (char)src[i];
    }
    dst[len] = '\0';
    return 0;
}

/* 建立 path 的所有上層目錄(已經存在的略過) */
static int make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int err = vfs_mkdir(path);
        *p = '/';
        if (err && err != -E_EXIST)
            return err;
    }
    return 0;
}

int initrd_unpack(const uint8_t *image, size_t len)
{
    int files = 0;

    for (size_t off = 0; off + BLK <= len; ) {
        const uint8_t *h = image + off;

        if (h[0] == '\0')
            break;                           /* 全 0 的區塊:結束 */
        if (memcmp(h + 257, "ustar", 5) != 0)
            return -E_INVAL;

        uint64_t size = octal(h + 124, 12);
        size_t data = off + BLK;
        if (size > len - data)
            return -E_INVAL;                 /* 內容超出這包資料的範圍 */

        /* 完整的名稱 = 前半段 + "/" + 名稱;統一成以 "/" 開頭的絕對路徑 */
        char path[FS_PATH_MAX + 1] = "/";
        if (h[345]) {
            if (append(path, sizeof(path), h + 345, 155) || append(path, sizeof(path), (const uint8_t *)"/", 1))
                return -E_NAMETOOLONG;
        }
        if (append(path, sizeof(path), h, 100))
            return -E_NAMETOOLONG;
        size_t plen = strlen(path);
        while (plen > 1 && path[plen - 1] == '/')
            path[--plen] = '\0';             /* 目錄的名稱結尾帶斜線,去掉 */

        char type = (char)h[156];
        if (type == '5') {
            int err = make_parents(path);
            if (err == 0 && plen > 1 && !(plen == 2 && path[1] == '.'))
                err = vfs_mkdir(path);
            if (err && err != -E_EXIST)
                return err;
        } else if (type == '0' || type == '\0') {
            struct file *f;
            int err = make_parents(path);
            if (err == 0)
                err = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &f);
            if (err)
                return err;
            long w = size ? vfs_write(f, image + data, (size_t)size) : 0;
            vfs_close(f);
            if (w != (long)size)
                return w < 0 ? (int)w : -E_NOMEM;
            /* 權限:/bin 底下的是程式(可以執行),其餘是資料。不採用檔案包裡記的權限 —
               那取決於建置時主機檔案系統的設定(在 Windows 的磁碟上每個檔案都被當成可執行),
               由這裡決定,結果才固定 */
            const char *rel = memcmp(path, "/./", 3) == 0 ? path + 2 : path;   /* 檔案包裡的名稱以 "./" 開頭 */
            vfs_chmod(path, memcmp(rel, "/bin/", 5) == 0 ? 0755 : 0644);
            files++;
        }
        /* 其他類型(符號連結等)目前不支援,略過 */

        off = data + (size_t)((size + BLK - 1) / BLK) * BLK;
    }
    return files;
}
