; =============================================================================
;  NodyOS boot loader  -  stage 1 (MBR, sector 0)  +  stage 2 (sectors 1..15)
;
;  Assembled as ONE flat binary. The BIOS loads sector 0 at 0000:7C00; stage 1
;  reads the remaining sectors right behind itself, so both stages share one
;  address space and one set of labels.
;
;  stage 2:   A20 gate
;          -> E820 memory map
;          -> load the kernel to 1 MiB (BIOS reads into a bounce buffer,
;             copied up in "unreal" mode)
;          -> VBE linear framebuffer (1024x768x32 preferred)
;          -> 32-bit protected mode, flat segments
;          -> jump to the kernel with EBX = boot info block
;
;  Disk layout (512-byte sectors):
;       0         stage 1
;       1 .. 15   stage 2
;      16 ..      kernel (flat binary, linked at 0x100000)
;    2048 ..      NodyFS volume (owned by the kernel, never touched here)
; =============================================================================

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 1
%endif

BOOT_SECTORS    equ 16
KERNEL_LBA      equ 16
KERNEL_ADDR     equ 0x100000
BOUNCE_SEG      equ 0x1000              ; 0x10000 .. 0x17FFF
CHUNK           equ 64                  ; sectors per BIOS read (32 KiB)

BOOTINFO        equ 0x5000              ; boot info block handed to the kernel
MMAP_BUF        equ 0x5100              ; E820 entries (24 bytes each)
VBE_INFO        equ 0x6000              ; VBE controller info (512 bytes)
VBE_MODE        equ 0x6400              ; VBE mode info (256 bytes)

        org 0x7C00
        bits 16

; -----------------------------------------------------------------------------
;  Stage 1
; -----------------------------------------------------------------------------
stage1:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        jmp     0x0000:s1_main          ; normalise CS
s1_main:
        sti
        cld
        mov     [boot_drive], dl

        mov     si, msg_boot
        call    print

        mov     ah, 0x41                ; INT 13h extensions present?
        mov     bx, 0x55AA
        mov     dl, [boot_drive]
        int     0x13
        jc      err_lba
        cmp     bx, 0xAA55
        jne     err_lba

        mov     word  [dap_count], BOOT_SECTORS - 1
        mov     word  [dap_off], 0x7E00
        mov     word  [dap_seg], 0
        mov     dword [dap_lba], 1
        call    disk_read
        jc      err_disk
        jmp     stage2

err_lba:
        mov     si, msg_lba
        jmp     fail
err_disk:
        mov     si, msg_disk
fail:
        call    print
halt:
        cli
        hlt
        jmp     halt

; print zero-terminated string at DS:SI with the BIOS teletype
print:
        lodsb
        test    al, al
        jz      print_done
        mov     ah, 0x0E
        mov     bx, 0x0007
        int     0x10
        jmp     print
print_done:
        ret

; read sectors described by the disk address packet; CF set on failure
disk_read:
        mov     byte [retries], 3
disk_retry:
        mov     si, dap
        mov     dl, [boot_drive]
        mov     ah, 0x42
        int     0x13
        jnc     disk_ok
        xor     ah, ah                  ; reset controller and try again
        mov     dl, [boot_drive]
        int     0x13
        dec     byte [retries]
        jnz     disk_retry
        stc
disk_ok:
        ret

boot_drive      db 0
retries         db 0

dap:            db 0x10, 0
dap_count:      dw 0
dap_off:        dw 0
dap_seg:        dw 0
dap_lba:        dd 0
                dd 0

msg_boot        db "NodyOS boot", 13, 10, 0
msg_lba         db "No LBA BIOS support", 0
msg_disk        db "Disk read error", 0

        times 510 - ($ - $$) db 0
        dw 0xAA55

; -----------------------------------------------------------------------------
;  Stage 2
; -----------------------------------------------------------------------------
stage2:
        mov     si, msg_load
        call    print

        ; ---- A20 gate --------------------------------------------------------
        mov     ax, 0x2401              ; BIOS
        int     0x15
        in      al, 0x92                ; fast A20
        test    al, 2
        jnz     a20_done
        or      al, 2
        and     al, 0xFE
        out     0x92, al
a20_done:

        ; ---- E820 memory map -------------------------------------------------
        xor     ebx, ebx
        xor     bp, bp
        mov     di, MMAP_BUF
e820_next:
        mov     eax, 0xE820
        mov     edx, 0x534D4150         ; 'SMAP'
        mov     ecx, 24
        int     0x15
        jc      e820_done
        cmp     eax, 0x534D4150
        jne     e820_done
        add     di, 24
        inc     bp
        cmp     bp, 32
        jae     e820_done
        test    ebx, ebx
        jnz     e820_next
e820_done:
        movzx   eax, bp
        mov     [BOOTINFO + 28], eax
        mov     dword [BOOTINFO + 24], MMAP_BUF

        ; ---- load the kernel to 1 MiB ---------------------------------------
        mov     dword [k_lba], KERNEL_LBA
        mov     dword [k_dest], KERNEL_ADDR
        mov     word  [k_left], KERNEL_SECTORS
load_next:
        mov     ax, [k_left]
        test    ax, ax
        jz      load_done
        cmp     ax, CHUNK
        jbe     load_cnt
        mov     ax, CHUNK
load_cnt:
        mov     [dap_count], ax
        mov     word [dap_off], 0
        mov     word [dap_seg], BOUNCE_SEG
        mov     eax, [k_lba]
        mov     [dap_lba], eax
        call    disk_read
        jc      err_disk

        call    unreal                  ; BIOS may have reset segment limits
        movzx   ecx, word [dap_count]
        shl     ecx, 7                  ; sectors * 512 / 4 dwords
        mov     esi, BOUNCE_SEG << 4
        mov     edi, [k_dest]
        cli
        a32 rep movsd                   ; DS:ESI -> ES:EDI, 4 GiB limits
        sti
        mov     [k_dest], edi

        movzx   eax, word [dap_count]
        add     [k_lba], eax
        sub     [k_left], ax

        mov     ax, 0x0E2E              ; progress dot
        mov     bx, 0x0007
        int     0x10
        jmp     load_next
load_done:

        ; ---- VBE linear framebuffer -----------------------------------------
        mov     di, VBE_INFO
        mov     dword [di], 'VBE2'
        mov     ax, 0x4F00
        int     0x10
        cmp     ax, 0x004F
        jne     err_vbe

        mov     ax, 1024
        mov     bx, 768
        mov     cl, 32
        call    find_mode
        jnc     vbe_set
        mov     ax, 800
        mov     bx, 600
        mov     cl, 32
        call    find_mode
        jnc     vbe_set
        mov     ax, 1024
        mov     bx, 768
        mov     cl, 24
        call    find_mode
        jnc     vbe_set
        mov     ax, 640
        mov     bx, 480
        mov     cl, 32
        call    find_mode
        jnc     vbe_set
err_vbe:
        mov     si, msg_vbe
        jmp     fail

vbe_set:
        mov     bx, [vbe_mode]
        or      bx, 0x4000              ; linear framebuffer
        mov     ax, 0x4F02
        int     0x10
        cmp     ax, 0x004F
        jne     err_vbe

        ; ---- boot info block -------------------------------------------------
        mov     dword [BOOTINFO + 0], 0x59444F4E        ; 'NODY'
        mov     eax, [VBE_MODE + 40]
        mov     [BOOTINFO + 4], eax                     ; framebuffer address
        movzx   eax, word [VBE_MODE + 18]
        mov     [BOOTINFO + 8], eax                     ; width
        movzx   eax, word [VBE_MODE + 20]
        mov     [BOOTINFO + 12], eax                    ; height
        movzx   eax, word [VBE_MODE + 16]
        mov     [BOOTINFO + 16], eax                    ; bytes per scanline
        movzx   eax, byte [VBE_MODE + 25]
        mov     [BOOTINFO + 20], eax                    ; bits per pixel
        movzx   eax, byte [boot_drive]
        mov     [BOOTINFO + 32], eax
        mov     dword [BOOTINFO + 36], KERNEL_SECTORS

        ; ---- protected mode --------------------------------------------------
        cli
        lgdt    [gdt_desc]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        jmp     0x08:pm_entry

; -----------------------------------------------------------------------------
;  find_mode: AX = width, BX = height, CL = bits per pixel
;  returns CF clear and [vbe_mode] set (mode info left in VBE_MODE), CF set if
;  no such linear-framebuffer mode exists.
; -----------------------------------------------------------------------------
find_mode:
        mov     [want_w], ax
        mov     [want_h], bx
        mov     [want_bpp], cl
        mov     si, [VBE_INFO + 14]     ; far pointer to the mode list
fm_next:
        mov     ax, [VBE_INFO + 16]
        mov     fs, ax
        mov     cx, [fs:si]
        add     si, 2
        cmp     cx, 0xFFFF
        je      fm_none
        mov     [vbe_mode], cx
        push    si
        mov     di, VBE_MODE
        mov     ax, 0x4F01
        int     0x10
        pop     si
        cmp     ax, 0x004F
        jne     fm_next
        mov     ax, [VBE_MODE + 0]      ; supported | graphics | linear fb
        and     ax, 0x0091
        cmp     ax, 0x0091
        jne     fm_next
        mov     ax, [VBE_MODE + 18]
        cmp     ax, [want_w]
        jne     fm_next
        mov     ax, [VBE_MODE + 20]
        cmp     ax, [want_h]
        jne     fm_next
        mov     al, [VBE_MODE + 25]
        cmp     al, [want_bpp]
        jne     fm_next
        cmp     byte [VBE_MODE + 27], 6 ; direct colour memory model
        jne     fm_next
        clc
        ret
fm_none:
        stc
        ret

; -----------------------------------------------------------------------------
;  unreal: give DS and ES 4 GiB limits while staying in real mode
; -----------------------------------------------------------------------------
unreal:
        cli
        push    ds
        push    es
        lgdt    [gdt_desc]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     short unreal_pm
unreal_pm:
        mov     bx, 0x10
        mov     ds, bx
        mov     es, bx
        and     al, 0xFE
        mov     cr0, eax
        jmp     short unreal_rm
unreal_rm:
        pop     es
        pop     ds
        sti
        ret

; -----------------------------------------------------------------------------
;  32-bit entry
; -----------------------------------------------------------------------------
        bits 32
pm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x90000
        mov     ebx, BOOTINFO
        mov     eax, KERNEL_ADDR
        jmp     eax

        bits 16

        align 8
gdt:
        dq 0
        dq 0x00CF9A000000FFFF           ; 0x08  code, base 0, limit 4 GiB, 32-bit
        dq 0x00CF92000000FFFF           ; 0x10  data, base 0, limit 4 GiB
gdt_desc:
        dw gdt_desc - gdt - 1
        dd gdt

k_lba           dd 0
k_dest          dd 0
k_left          dw 0
want_w          dw 0
want_h          dw 0
want_bpp        db 0
vbe_mode        dw 0

msg_load        db "Loading kernel", 0
msg_vbe         db 13, 10, "No usable VBE graphics mode", 0

        times BOOT_SECTORS * 512 - ($ - $$) db 0
