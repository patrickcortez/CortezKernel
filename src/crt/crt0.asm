; =============================================================================
; @file crt0.asm
; @brief 64-bit Higher-Half Kernel Entry Point (_start) for the Cortez Kernel.
; =============================================================================

bits 64
default rel

section .bss
align 16
global g_bootStackBottom
global g_bootStackTop
g_bootStackBottom:
    resb 65536 ; 64 KiB kernel bootstrap stack (PID 0: swapper)
g_bootStackTop:

section .text
global _start
extern __cortez_call_global_constructors
extern start_kernel

_start:
    cli
    cld

    ; Set up 16-byte aligned initial kernel stack
    lea rsp, [g_bootStackTop]
    xor rbp, rbp

    ; Invoke C++ global constructors (.init_array)
    call __cortez_call_global_constructors

    ; Enter Linux-style C++ kernel orchestrator
    call start_kernel

.halt_loop:
    cli
    hlt
    jmp .halt_loop
