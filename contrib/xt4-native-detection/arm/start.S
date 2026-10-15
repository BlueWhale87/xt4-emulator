.syntax unified
.arm
.section .text.start,"ax",%progbits
.global _start
.type _start,%function
_start:
    cpsid if
    ldr r0, =__bss_start__
    ldr r1, =__bss_end__
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    ldr r0, =__data_load__
    ldr r1, =__data_start__
    ldr r2, =__data_end__
2:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 2b
    ldr r0, =__stack_bottom__
    ldr r1, =__stack_top__
    ldr r2, =0xa5a5a5a5
3:  cmp r0, r1
    strlo r2, [r0], #4
    blo 3b
    ldr sp, =__stack_top__
    bl xt4_smoke_main
    bl xt4_semihost_exit
4:  b 4b
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
