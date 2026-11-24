#ifndef WELYOS_AFFINITY_CHECK_H
#define WELYOS_AFFINITY_CHECK_H
/*
 * SYS_setaffinity 的參數檢查(I-344):純函式,不碰任何 kernel 狀態,主機端測試可以直接 include
 * (Codex 的參數驗證測試對它寫)。只檢查輸入規則;權限與「查詢、檢查、設定在同一段鎖裡」的原子性
 * 由 kernel 端(thread_set_affinity_checked)負責,不在這裡。
 *
 * raw 是使用者傳進來的 long 轉成 unsigned long:
 *   - 0:空遮罩,-E_INVAL
 *   - 超過 32 位元(64 位元架構上的負數或大數):-E_INVAL(轉成 unsigned 之前先擋,否則截斷後
 *     可能變成合法的遮罩)
 *   - 含不在線上的 CPU(online 以外的位元):-E_INVAL
 * 合格時 *out = 遮罩,回傳 0。i386 的 long 是 32 位元:第 31 顆 CPU 的位元在 long 裡是負數,
 * 轉成 unsigned long 後仍是同一個 32 位元值,不會被誤擋(MAX_CPUS 目前是 8)。
 */
#include "uapi/errno.h"

static inline int affinity_mask_check(unsigned long raw, unsigned online, unsigned *out)
{
    if (raw == 0 || (raw >> 16 >> 16) != 0)    /* 分兩次移位:32 位元的 long 上移 32 位是未定義行為 */
        return -E_INVAL;
    unsigned m = (unsigned)raw;
    if (m & ~online)
        return -E_INVAL;
    *out = m;
    return 0;
}

#endif
