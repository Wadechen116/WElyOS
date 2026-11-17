/*
 * switch.S (i386) - context switch
 *
 * void arch_switch(void **old_sp, void *new_sp);
 *
 * 只需要存「callee-saved」暫存器(ebx、esi、edi、ebp):依 C 呼叫慣例,
 * 其餘暫存器(eax、ecx、edx)呼叫者本來就預期函式呼叫後會被改掉,
 * 編譯器在呼叫 arch_switch 之前已自行保存需要的部分。
 * eip 不用另外存:呼叫 arch_switch 時 call 指令已把返回位址推上 stack。
 *
 * 切換出去時 stack 的樣子(低位址在上),arch_stack_init 要仿造同樣的版面:
 *     edi esi ebx ebp   <- 這裡推入
 *     返回位址          <- call 推入
 */
    .section .text
    .global arch_switch
arch_switch:
    mov     4(%esp), %eax         /* old_sp:存放舊 stack pointer 的位置 */
    mov     8(%esp), %edx         /* new_sp:要切換過去的 stack pointer */
    push    %ebp
    push    %ebx
    push    %esi
    push    %edi
    mov     %esp, (%eax)          /* 記下舊執行緒的 stack pointer */
    mov     %edx, %esp            /* ---- 從這一行起,已經在新執行緒的 stack 上 ---- */
    pop     %edi
    pop     %esi
    pop     %ebx
    pop     %ebp
    ret                           /* 彈出新執行緒的返回位址,回到它上次切出去的地方 */
