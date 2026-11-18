/*
 * sync.c - 把還留在記憶體裡的修改寫到磁碟上
 * 用法:sync
 *
 * 平常不需要:檔案關閉、建立、刪除之後,kernel 就會寫回磁碟,關機時也會。
 * 它的用途是「現在就要確定都寫進去了」,例如準備強制結束模擬器之前。
 */
#include "ulib.h"

int main(void)
{
    if (sys_sync() != 0) {
        puts_("sync: write error\n");
        return 1;
    }
    return 0;
}
