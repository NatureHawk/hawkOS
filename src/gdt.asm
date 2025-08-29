BITS 32

gdt_start:

;Null descriptor 
gdt_null: 
    dq 0               

; 0x08 — Code segment descriptor
gdt_code:
    dw 0xFFFF           ; Limit (bits 0–15)
    dw 0x0000           ; Base (bits 0–15)
    db 0x00             ; Base (bits 16–23)
    db 0x9A             ; Access byte (code, ring 0, present)
    db 0xCF             ; Flags + Limit high bits (4KB granularity, 32-bit)
    db 0x00             ; Base (bits 24–31)

; 0x10 — Data segment descriptor
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92             ; Access byte (data, ring 0, present)
    db 0xCF
    db 0x00

gdt_end:

gdtr:
    dw gdt_end - gdt_start - 1   ; Size of GDT (limit = size - 1)
    dd gdt_start                 ; Address of GDT in memory


global load_gdt
load_gdt:
    lgdt [gdtr]                  ; Load address & size of GDT into CPU

    ; Reload segment registers with new GDT entries
    mov ax, 0x10                 ; Data segment selector (index 2 = gdt_data)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;jump to reload CS with selector 0x08
    jmp 0x08:flush
flush:
    ret
