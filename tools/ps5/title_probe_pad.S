/* PAD_MIB MiB of non-zero data in .rodata, to see whether the console starts a title of that size. */
    .section .rodata
    .globl pad
    .globl pad_end
    .balign 4096
pad:
    .fill PAD_MIB * 1024 * 1024, 1, 0xa5
pad_end:
