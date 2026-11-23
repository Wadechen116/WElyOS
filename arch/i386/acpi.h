#ifndef WELYOS_I386_ACPI_H
#define WELYOS_I386_ACPI_H
#include <stdint.h>

/* 依 4 字元簽章找 ACPI 表格(含 36 位元組標頭,已檢查長度與檢查碼);沒有則 NULL。見 smp.c */
const uint8_t *acpi_find_table(const char sig[4], uint32_t *len);

#endif
