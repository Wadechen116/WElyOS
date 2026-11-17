/*
 * crt0.c - 每個使用者程式都會連結的啟動程式(C runtime 的第 0 部分)
 *
 * kernel 把 CPU 降到使用者模式後,從 _start 開始執行,並傳入一個指標,
 * 指向 kernel 放在 stack 頂端的參數區塊(格式見 kernel/proc.c 的 image_put_args):
 *
 *     args[0]           argc(參數個數)
 *     args[1 .. argc]   argv[0] .. argv[argc-1](字串的位址)
 *     args[argc + 1]    NULL
 *
 * 這裡把它轉成 C 程式習慣的 main(argc, argv),main 返回後以它的回傳值結束行程。
 * 使用者程式沒有「上一層」可以返回,所以 _start 絕對不能返回。
 */
#include "usys.h"

int main(int argc, char **argv);

/* 放在 .text.start 區段:linker script 把它排在程式碼的最前面 */
__attribute__((section(".text.start"), noreturn))
void _start(long *args)
{
    sys_exit(main((int)args[0], (char **)&args[1]));
}

/*
 * 編譯器在最佳化時可能自行產生對 memset / memcpy 的呼叫(例如把結構清為 0)。
 * 使用者程式沒有連結任何函式庫,也不能用 kernel 裡的那一份,所以要自己提供。
 */
void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}
