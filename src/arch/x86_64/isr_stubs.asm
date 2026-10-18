; =============================================================================
; @file isr_stubs.asm
; @brief 256 x86_64 Interrupt Service Routine (ISR) stubs and common dispatcher.
; =============================================================================

bits 64
default rel

section .text
extern cortez_interrupt_dispatch

%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0         ; Dummy error code
    push qword %1        ; Interrupt vector
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push qword %1        ; Interrupt vector (CPU already pushed error code)
    jmp isr_common_stub
%endmacro

; CPU Exceptions 0..31
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs and MSI Vectors 32..255
%assign i 32
%rep 224
    ISR_NOERR i
%assign i i+1
%endrep

isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp         ; 1st argument: InterruptFrame*
    mov rbp, rsp
    and rsp, -16         ; 16-byte stack alignment per System V AMD64 ABI
    call cortez_interrupt_dispatch
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16          ; Pop vector and errorCode
    iretq

section .rodata
align 8
global g_isrStubTable
g_isrStubTable:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
