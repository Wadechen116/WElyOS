/*
 * trap.S (i386) - 例外入口與 GDT 載入
 *
 * x86 的例外機制:CPU 以「向量編號」(0~255)查 IDT(中斷描述符表),
 * 找到對應的入口位址後,硬體自動在目前 stack 上推入:
 *     eflags、cs、eip        (同一權限等級時;由低權限進入時還會多推 ss、esp)
 *     錯誤碼(error code)   <-- 只有部分例外才有(8、10~14、17、21、29、30)
 * 然後跳到入口。硬體不會存一般暫存器。
 *
 * 兩個麻煩之處,這裡用巨集解決:
 *   1. CPU 不會告訴入口「我是第幾號向量」,所以每個向量要有自己的小入口,
 *      由它把編號推上 stack。硬體中斷(向量 32~47)也用同樣的入口,沒有錯誤碼。
 *   2. 有的例外有錯誤碼、有的沒有。沒有的就自己推一個 0,讓 stack 版面統一。
 *
 * 統一後的 stack(低位址在上),即 trap.h 的 struct trap_frame:
 *     es ds                             <- isr_common 推入
 *     edi esi ebp esp ebx edx ecx eax   <- pusha 推入
 *     vector  err                       <- 小入口推入
 *     eip cs eflags                     <- CPU 推入
 *     esp ss                            <- CPU 推入,「只有」從使用者模式(ring 3)進來時才有
 *
 * 從使用者模式進來時,CPU 在推入任何東西之前會先換 stack:從 TSS 取出 ss0:esp0
 * 作為新的 stack(也就是目前執行緒的 kernel stack 頂端),再把使用者原本的 ss、esp
 * 推上去。這是硬體強制的:kernel 絕不在使用者提供的 stack 上執行。
 * iret 返回時,看到要回去的 cs 是 ring 3,就會多彈出 esp 與 ss。
 */

/* 沒有錯誤碼的例外:補一個 0 */
.macro ISR_NOERR n
isr\n:
    push    $0
    push    $\n
    jmp     isr_common
.endm

/* 有錯誤碼的例外:CPU 已推入錯誤碼,只需再推向量編號 */
.macro ISR_ERR n
isr\n:
    push    $\n
    jmp     isr_common
.endm

    .section .text
    .irp    n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \n
    .endr
    .global isr128                /* 向量 0x80:系統呼叫(使用者程式以 int $0x80 進入) */
    ISR_NOERR 128
    .irp    n, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \n
    .endr

isr_common:
    pusha                         /* 存 eax ecx edx ebx esp ebp esi edi */
    /* 資料區段暫存器:從使用者模式進來時它們還是使用者的區段,
       存起來並換成 kernel 的資料區段(cs 與 ss 已由 CPU 換好) */
    push    %ds
    push    %es
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    cld                           /* C 呼叫慣例要求方向旗標為 0 */
    push    %esp                  /* 參數:指向 trap frame 的指標 */
    call    trap_handler          /* 可修改 frame(例如調整 eip、放入系統呼叫的回傳值) */
    add     $4, %esp              /* 丟掉參數 */
    pop     %es
    pop     %ds
    popa                          /* 還原一般暫存器 */
    add     $8, %esp              /* 丟掉 vector 與 err */
    iret                          /* 彈出 eip、cs、eflags(回 ring 3 時還有 esp、ss) */

/*
 * void arch_enter_user(uintptr_t entry, uintptr_t usp, uintptr_t arg)
 * 第一次進入使用者模式。x86 沒有「降低權限」的指令,做法是在 stack 上擺出
 * 和「從使用者模式被中斷」一模一樣的五個值,再執行 iret,讓 CPU 以為是返回。
 * 選擇子 0x2B = GDT 第 5 項 | RPL 3(使用者程式碼),0x33 = 第 6 項 | RPL 3(使用者資料)。
 */
    .global arch_enter_user
arch_enter_user:
    mov     4(%esp), %ecx         /* entry */
    mov     8(%esp), %edx         /* usp */
    mov     12(%esp), %eax        /* arg */
    /* 依 C 呼叫慣例在使用者 stack 上放好參數:[usp] = 假的返回位址,[usp+4] = arg。
       此時已載入該行程的頁表,kernel 可以直接寫入它的 stack */
    sub     $8, %edx
    /* 啟用 SMAP 時,kernel 存取使用者頁會被擋下;stac 暫時解除(設定 eflags.AC),
       寫完立刻用 clac 恢復。沒有 SMAP 的 CPU 不認得這兩條指令,所以先檢查 */
    cmpl    $0, smap_enabled
    je      1f
    stac
1:  movl    $0, (%edx)
    mov     %eax, 4(%edx)
    cmpl    $0, smap_enabled
    je      2f
    clac
2:
    mov     $0x33, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    push    $0x33                 /* ss */
    push    %edx                  /* esp */
    push    $0x202                /* eflags:IF = 1(開中斷),bit 1 恆為 1 */
    push    $0x2B                 /* cs */
    push    %ecx                  /* eip */
    /* 清掉一般暫存器:不把 kernel 內部的數值(可能是位址)洩漏給使用者程式 */
    xor     %eax, %eax
    xor     %ebx, %ebx
    xor     %ecx, %ecx
    xor     %edx, %edx
    xor     %esi, %esi
    xor     %edi, %edi
    xor     %ebp, %ebp
    iret

/*
 * void gdt_flush(const void *gdt_ptr)
 * 載入新的 GDT 並讓所有區段暫存器改用它。
 * 資料區段可直接 mov;cs 不能 mov,必須用遠跳躍(ljmp)才會重新載入。
 * 選擇子 0x08 = GDT 第 1 項(kernel 程式碼),0x10 = 第 2 項(kernel 資料)。
 */
    .global gdt_flush
gdt_flush:
    mov     4(%esp), %eax
    lgdt    (%eax)
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss
    ljmp    $0x08, $1f
1:  ret

/* 48 個入口的位址表(0~31 CPU 例外、32~47 硬體中斷 IRQ 0~15),供 trap.c 填入 IDT */
    .section .rodata
    .global isr_table
    .align  4
isr_table:
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long   isr\n
    .endr
