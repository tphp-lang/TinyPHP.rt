    .text
    .globl __chkstk
__chkstk:
    pushq %rcx
    pushq %r11
    movq %rsp, %r11
    addq $16, %r11
    movq %rax, %rcx
1:
    cmpq $0x1000, %rcx
    jbe 2f
    subq $0x1000, %rcx
    subq $0x1000, %r11
    cmpb $0, (%r11)
    jmp 1b
2:
    popq %r11
    popq %rcx
    ret
