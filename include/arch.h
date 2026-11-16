#ifndef WELYOS_ARCH_H
#define WELYOS_ARCH_H

/*
 * 架構抽象介面。kernel/ 只透過這些函式與硬體互動,
 * 每個架構(arch/<名稱>/arch.c)各自實作。
 */

void arch_init(void);                          /* 初始化主控台,之後才能 kprintf */
void arch_putc(char c);                        /* 輸出一個字元到主控台 */
void arch_halt(void) __attribute__((noreturn));/* 讓 CPU 永久閒置 */
void arch_exit(int code) __attribute__((noreturn)); /* 結束模擬器(僅測試用) */
const char *arch_name(void);                   /* 架構名稱字串 */
#endif
