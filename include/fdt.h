#ifndef WELYOS_FDT_H
#define WELYOS_FDT_H
#include "boot_info.h"

/*
 * 解析裝置樹(Flattened Device Tree, DTB),填入 boot_info:
 *   - /chosen 的 bootargs      -> cmdline
 *   - /memory 的第一組 reg     -> mem_base / mem_size
 *   - DTB 本身與記憶體保留表   -> reserved[]
 *   - /cpus 底下各 cpu@N 的 reg -> cpu_ids[] / ncpus
 * 成功回傳 0;DTB 魔術數不符回傳 -1(此時 bi 被設為空值)。
 */
int fdt_parse(const void *fdt, struct boot_info *bi);
#endif
