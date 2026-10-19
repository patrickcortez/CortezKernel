; =============================================================================
; @file context_switch.asm
; @brief x86_64 System V AMD64 ABI callee-saved + FPU/SSE context switch.
; =============================================================================

bits 64
default rel

section .text
global cortez_switch_context
global cortez_enable_fpu_sse
global cortez_fxsave
global cortez_fxrstor

; void cortez_enable_fpu_sse()
cortez_enable_fpu_sse:
    mov rax, cr0
    and rax, ~(1 << 2)      ; Clear CR0.EM
    or  rax, (1 << 1) | (1 << 5) ; Set CR0.MP | CR0.NE
    mov cr0, rax

    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10) ; Set CR4.OSFXSR | CR4.OSXMMEXCPT
    mov cr4, rax

    fninit
    sub rsp, 8
    mov dword [rsp], 0x1F80 ; Default MXCSR (all SIMD exceptions masked)
    ldmxcsr [rsp]
    add rsp, 8
    ret

; void cortez_fxsave(void* state512)
cortez_fxsave:
    test rdi, rdi
    jz .done
    fxsave [rdi]
.done:
    ret

; void cortez_fxrstor(const void* state512)
cortez_fxrstor:
    test rdi, rdi
    jz .done
    fxrstor [rdi]
.done:
    ret

; void cortez_switch_context(uint64_t* prevRspOut, uint64_t nextRsp, void* prevFx, const void* nextFx)
; rdi = prevRspOut, rsi = nextRsp, rdx = prevFx, rcx = nextFx
cortez_switch_context:
    test rdx, rdx
    jz .skip_save
    fxsave [rdx]
.skip_save:
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

    test rcx, rcx
    jz .skip_restore
    fxrstor [rcx]
.skip_restore:
    ret
