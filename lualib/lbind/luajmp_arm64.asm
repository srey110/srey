; 不逐帧展开的 setjmp / longjmp（Windows ARM64），给 luauser.h 的 LUAI_TRY / LUAI_THROW 用。
; 只存取被调用方保存的寄存器：x19~x30、sp、d8~d15，共 168 字节，放得进 jmp_buf(192)；不碰平台保留的 x18
    AREA |.text|, CODE, READONLY, ALIGN=4

    EXPORT luajmp_save
luajmp_save PROC
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret
    ENDP

    EXPORT luajmp_restore
luajmp_restore PROC
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne ; v 为 0 时返回 1，同 longjmp
    ret
    ENDP
    END
