/*
 * userimg.S - 把使用者程式的二進位映像嵌入 kernel
 *
 * USER_IMG_PATH 由建置系統定義,指向 user/init.c 連結並轉成原始二進位後的檔案。
 * 放在 .rodata:kernel 只讀取它(複製到新行程的頁面)。
 * 目前還沒有檔案系統,這是讓 kernel 取得使用者程式最簡單的方式。
 */
    .section .rodata
    .balign 16
    .global user_image_start
user_image_start:
    .incbin USER_IMG_PATH
    .global user_image_end
user_image_end:
