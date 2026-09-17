
option casemap:none

EXTERN bridger_ffi_dispatch:PROC

HOOK_SLOTS EQU 256

.code

bridger_ffi_call PROC FRAME
    push rbp
    .pushreg rbp
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    mov r10, rcx
    mov rsi, rdx
    mov rdi, r8
    mov rbx, r9
    mov r12, [rbp + 80]

    mov rax, rbx
    cmp rax, 4
    jae have_count
    mov rax, 4
have_count:
    shl rax, 3
    add rax, 15
    and rax, -16
    sub rsp, rax

    mov rcx, 4
copy_stack:
    cmp rcx, rbx
    jae stack_done
    mov rax, [rsi + rcx*8]
    mov [rsp + rcx*8], rax
    inc rcx
    jmp copy_stack
stack_done:

    movsd xmm0, qword ptr [rdi]
    movsd xmm1, qword ptr [rdi + 8]
    movsd xmm2, qword ptr [rdi + 16]
    movsd xmm3, qword ptr [rdi + 24]
    mov rcx, [rsi]
    mov rdx, [rsi + 8]
    mov r8, [rsi + 16]
    mov r9, [rsi + 24]
    call r10

    movsd qword ptr [r12], xmm0

    lea rsp, [rbp]
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret
bridger_ffi_call ENDP

hook_common PROC FRAME
    push rbp
    .pushreg rbp
    sub rsp, 80
    .allocstack 80
    .endprolog

    mov [rsp + 96], rcx
    mov [rsp + 104], rdx
    mov [rsp + 112], r8
    mov [rsp + 120], r9
    movsd qword ptr [rsp + 32], xmm0
    movsd qword ptr [rsp + 40], xmm1
    movsd qword ptr [rsp + 48], xmm2
    movsd qword ptr [rsp + 56], xmm3
    mov qword ptr [rsp + 64], 0

    mov ecx, eax
    lea rdx, [rsp + 96]
    lea r8, [rsp + 32]
    lea r9, [rsp + 64]
    call bridger_ffi_dispatch

    movsd xmm0, qword ptr [rsp + 64]
    add rsp, 80
    pop rbp
    ret
hook_common ENDP

HOOK_STUB MACRO index
bridger_ffi_hook_&index& PROC
    mov eax, index
    jmp hook_common
bridger_ffi_hook_&index& ENDP
ENDM

slot = 0
WHILE slot LT HOOK_SLOTS
    HOOK_STUB %slot
    slot = slot + 1
ENDM

CONST SEGMENT READ ALIGN(8) 'CONST'

HOOK_ENTRY MACRO index
    dq bridger_ffi_hook_&index&
ENDM

PUBLIC bridger_ffi_hook_table
bridger_ffi_hook_table LABEL QWORD
slot = 0
WHILE slot LT HOOK_SLOTS
    HOOK_ENTRY %slot
    slot = slot + 1
ENDM

CONST ENDS

END
