; =============================================================================
;  NodyOS kernel entry + interrupt stubs (32-bit protected mode)
;  The boot loader jumps here with EBX = boot info block.
; =============================================================================
        bits 32

        extern kmain
        extern isr_handler
        extern __bss_start
        extern __bss_end

        section .text.entry
        global _start
_start:
        cli
        cld

        mov     edi, __bss_start        ; clear .bss (the stack lives there too)
        mov     ecx, __bss_end
        sub     ecx, edi
        shr     ecx, 2
        xor     eax, eax
        rep stosd

        mov     esp, kstack_top

        lgdt    [gdt_ptr]               ; kernel-owned GDT
        jmp     0x08:.cs
.cs:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax

        mov     eax, cr0                ; enable x87 + SSE
        and     eax, 0xFFFFFFF3         ; clear EM, TS
        or      eax, 0x00000022         ; set MP, NE
        mov     cr0, eax
        mov     eax, cr4
        or      eax, 0x00000600         ; OSFXSR | OSXMMEXCPT
        mov     cr4, eax
        fninit
        push    dword 0x9F80            ; all SSE exceptions masked, flush-to-zero
        ldmxcsr [esp]
        add     esp, 4

        sub     esp, 12                 ; keep the stack 16-byte aligned at the call
        push    ebx
        call    kmain
.hang:
        cli
        hlt
        jmp     .hang

; -----------------------------------------------------------------------------
;  Interrupt stubs: vectors 0..31 are CPU exceptions, 32..47 are IRQ 0..15
; -----------------------------------------------------------------------------
        section .text

%macro ISR_NOERR 1
isr_stub_%1:
        push    dword 0
        push    dword %1
        jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
        push    dword %1
        jmp     isr_common
%endmacro

%assign i 0
%rep 48
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
        ISR_ERR i
  %else
        ISR_NOERR i
  %endif
  %assign i i+1
%endrep

; Saves the integer registers and the full FPU/SSE state, so interrupt handlers
; written in C can never corrupt floating point work they interrupt.
isr_common:
        pusha
        mov     ebp, esp                ; ebp -> saved register frame
        sub     esp, 512
        and     esp, 0xFFFFFFF0
        fxsave  [esp]
        mov     esi, esp
        sub     esp, 12
        push    ebp
        cld
        call    isr_handler
        fxrstor [esi]
        mov     esp, ebp
        popa
        add     esp, 8                  ; vector number + error code
        iretd

        section .data
        global isr_table
isr_table:
%assign i 0
%rep 48
        dd isr_stub_ %+ i
  %assign i i+1
%endrep

        align 8
gdt:
        dq 0
        dq 0x00CF9A000000FFFF           ; 0x08 kernel code
        dq 0x00CF92000000FFFF           ; 0x10 kernel data
gdt_ptr:
        dw 23
        dd gdt

        section .bss
        align 16
kstack:
        resb 262144
kstack_top:
