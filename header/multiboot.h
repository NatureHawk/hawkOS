#pragma once
#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_FLAG_MEM   0x001   // mem_lower / mem_upper valid
#define MULTIBOOT_FLAG_MMAP  0x040   // mmap_addr / mmap_length valid
#define MULTIBOOT_FLAG_FB    0x1000  // framebuffer_* fields valid

#define MULTIBOOT_FB_TYPE_RGB 1

typedef struct __attribute__((packed)) {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];   // a.out or ELF section-header info union; only the size matters here

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    // colour-info union (palette or RGB field positions) follows; unused here.
} multiboot_info_t;

// One entry in the BIOS memory map. NOTE: `size` describes the bytes that
// follow it (addr/len/type), so advance by size + sizeof(size), not sizeof(entry).
typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;   // 1 = available RAM, anything else = reserved/unusable
} multiboot_mmap_entry_t;

#define MULTIBOOT_MEMORY_AVAILABLE 1
