; =============================================================================
; @file context_switch.asm
; @brief x86_64 System V AMD64 ABI callee-saved context switch (switch_to).
; =============================================================================

bits 64
default rel

section .text
global cortez_switch_context

; void cortez_switch_context(uint64_t* prevRspOut, uint64_t nextRsp)
; rdi = prevRspOut, rsi = nextRsp
cortez_switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
