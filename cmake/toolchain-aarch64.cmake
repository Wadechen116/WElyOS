# AArch64 交叉編譯器(apt: gcc-aarch64-linux-gnu)。
# 雖然名稱含 linux-gnu,搭配 -ffreestanding -nostdlib 即為裸機用途。
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
