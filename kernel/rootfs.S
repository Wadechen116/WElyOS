/*
 * rootfs.S - 把開機檔案包(tar)嵌入 kernel
 *
 * ROOTFS_PATH 由建置系統定義,指向打包好的 rootfs.tar(內容見 CMakeLists.txt)。
 * 放在 .rodata:kernel 只讀取它,開機時解開到記憶體檔案系統(kernel/initrd.c)。
 */
    .section .rodata
    .balign 16
    .global rootfs_start
rootfs_start:
    .incbin ROOTFS_PATH
    .global rootfs_end
rootfs_end:
