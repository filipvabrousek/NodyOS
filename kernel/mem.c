/* NodyOS - kernel heap: first-fit free list with coalescing. */
#include "kernel/kernel.h"

extern u8 __bss_end[];

typedef struct Block {
    usize size;                 /* payload bytes */
    struct Block *next;         /* next block by address */
    u32 free;
    u32 magic;
} Block;                        /* 16 bytes: payloads stay 16-byte aligned */

#define BLOCK_MAGIC 0x4B4C4248u
#define MIN_SPLIT   64

static Block *heap;
static usize heap_bytes, used_bytes;

void mem_init(const BootInfo *bi)
{
    uptr start = ((uptr)__bss_end + 4095) & ~(uptr)4095;
    uptr end = 0x04000000;                              /* fallback: assume 64 MiB */
    const E820 *map = (const E820 *)(uptr)bi->mmap_addr;
    for (u32 i = 0; i < bi->mmap_count; i++) {
        if (map[i].type != 1 || map[i].base_hi) continue;
        u32 base = map[i].base_lo;
        u64 top = (u64)base + (((u64)map[i].len_hi << 32) | map[i].len_lo);
        if (base <= 0x100000 && top > start) {
            if (top > 0x40000000ull) top = 0x40000000ull;       /* use at most 1 GiB */
            end = (uptr)top;
        }
    }
    end &= ~(uptr)4095;
    heap = (Block *)start;
    heap->size = end - start - sizeof(Block);
    heap->next = NULL;
    heap->free = 1;
    heap->magic = BLOCK_MAGIC;
    heap_bytes = heap->size;
    used_bytes = 0;
    kprintf("mem: heap %x..%x (%u MiB)\n", (u32)start, (u32)end, (u32)((end - start) >> 20));
}

void *kmalloc(usize n)
{
    n = (n + 15) & ~(usize)15;
    if (!n) n = 16;
    for (Block *b = heap; b; b = b->next) {
        if (!b->free) continue;
        while (b->next && b->next->free) {              /* merge with free successors */
            b->size += sizeof(Block) + b->next->size;
            b->next = b->next->next;
        }
        if (b->size < n) continue;
        if (b->size >= n + sizeof(Block) + MIN_SPLIT) {
            Block *rest = (Block *)((u8 *)(b + 1) + n);
            rest->size = b->size - n - sizeof(Block);
            rest->next = b->next;
            rest->free = 1;
            rest->magic = BLOCK_MAGIC;
            b->size = n;
            b->next = rest;
        }
        b->free = 0;
        used_bytes += b->size;
        return b + 1;
    }
    kprintf("mem: out of memory allocating %u bytes\n", (u32)n);
    return NULL;
}

void kfree(void *p)
{
    if (!p) return;
    Block *b = (Block *)p - 1;
    if (b->magic != BLOCK_MAGIC || b->free) panic("kfree: bad pointer");
    b->free = 1;
    used_bytes -= b->size;
}

void plat_meminfo(u32 *total_kb, u32 *used_kb)
{
    *total_kb = (u32)(heap_bytes >> 10);
    *used_kb = (u32)(used_bytes >> 10);
}
