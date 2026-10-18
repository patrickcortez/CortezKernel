; =============================================================================
; @file gdt_flush.asm
; @brief Loads the 64-bit GDT (lgdt), reloads CS/DS/ES/SS, and loads TSS (ltr).
; =============================================================================

bits 64
default rel

section .text
global cortez_gdt_load

; void cortez_gdt_load(const GdtDescriptor* gdtr, uint16_t cs, uint16_t ds, uint16_t tssSel)
; System V AMD64 ABI: rdi = gdtr, rsi = cs, rdx = ds, rcx = tssSel
cortez_gdt_load:
    lgdt [rdi]

    ; Reload data segment registers
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    ; Far return (retfq) to reload CS with 64-bit Kernel Code Segment
    push rsi
    lea rax, [.reload_cs]
    push rax
    retfq

.reload_cs:
    ; Load Task Register with TSS selector
    mov ax, cx
    ltr ax
    ret
