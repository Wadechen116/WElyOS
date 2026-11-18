/*
 * scrub.c - 把整顆磁碟巡檢一遍
 * 用法:scrub
 *
 * 磁碟上每個區塊都有檢查碼。平常只有「讀到」的區塊會被核對;很久沒被用到的區塊
 * 就算壞了也沒有人知道。巡檢把每個區塊都直接從磁碟讀一次並核對,
 * 在資料還沒被用到之前就發現損壞。(kernel 也有一條背景執行緒慢慢地做同一件事。)
 * 回傳 0 = 全部正確;1 = 發現損壞的區塊;2 = 無法巡檢。
 */
#include "libc.h"

int main(void)
{
    struct scrubinfo si;
    long rc = sys_scrub(&si);

    if (rc != 0) {
        fprintf(2, "scrub: %s\n", rc == -E_PERM ? "permission denied" : "failed");
        return 2;
    }
    if (!si.checksums) {
        printf("scrub: no disk with checksums (disks formatted by older versions have none)\n");
        return 2;
    }
    printf("scrub: %u blocks, %u corrupted; %u read(s) refused since boot%s\n", si.blocks, si.bad,
           si.read_errors, si.sb_restored ? "; the superblock was restored from its backup at boot" : "");
    return si.bad ? 1 : 0;
}
