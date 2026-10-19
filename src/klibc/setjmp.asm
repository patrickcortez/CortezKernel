; =============================================================================
; @file setjmp.asm
; @brief x86_64 System V AMD64 ABI freestanding setjmp / longjmp implementation.
; =============================================================================

bits 64
default rel

section .text
global cortez_setjmp
global cortez_longjmp

; int cortez_setjmp(uint64_t env[8])
; rdi = env buffer: [0]=rbx, [1]=rbp, [2]=r12, [3]=r13, [4]=r14, [5]=r15, [6]=rsp, [7]=rip
cortez_setjmp:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rdx, [rsp + 8]      ; caller's rsp before call
    mov [rdi + 48], rdx
    mov rdx, [rsp]          ; return address (rip)
    mov [rdi + 56], rdx
    xor eax, eax            ; return 0 on direct call
    ret

; void cortez_longjmp(uint64_t env[8], int val)
; rdi = env buffer, esi = val
cortez_longjmp:
    mov eax, esi
    test eax, eax
    jnz .non_zero
    mov eax, 1              ; longjmp must never return 0
.non_zero:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rdx, [rdi + 56]
    jmp rdx
