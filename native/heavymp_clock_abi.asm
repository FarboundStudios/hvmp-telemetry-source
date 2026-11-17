; ETS2 callers retain volatile registers across its internal clock leaf function.
; Preserve them around the C++ bridge, including the original caller address.
OPTION CASEMAP:NONE
EXTERN HeavyMpDisplayTimeBridge:PROC
.code
HeavyMpDisplayTimeThunk PROC FRAME
    sub rsp, 0C8h
    .allocstack 0C8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    mov [rsp+40h], r10
    mov [rsp+48h], r11
    movdqu [rsp+50h], xmm0
    movdqu [rsp+60h], xmm1
    movdqu [rsp+70h], xmm2
    movdqu [rsp+80h], xmm3
    movdqu [rsp+90h], xmm4
    movdqu [rsp+0A0h], xmm5
    mov rdx, [rsp+0C8h]
    call HeavyMpDisplayTimeBridge
    movdqu xmm0, [rsp+50h]
    movdqu xmm1, [rsp+60h]
    movdqu xmm2, [rsp+70h]
    movdqu xmm3, [rsp+80h]
    movdqu xmm4, [rsp+90h]
    movdqu xmm5, [rsp+0A0h]
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    mov r10, [rsp+40h]
    mov r11, [rsp+48h]
    add rsp, 0C8h
    ret
HeavyMpDisplayTimeThunk ENDP
END
