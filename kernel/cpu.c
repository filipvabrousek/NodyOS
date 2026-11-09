/* NodyOS - interrupt descriptor table, PIC, timer, exceptions. */
#include "kernel/kernel.h"

struct idt_entry {
    u16 off_lo, sel;
    u8  zero, flags;
    u16 off_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

extern u32 isr_table[48];

static struct idt_entry idt[256];
static void (*irq_fn[16])(void);
volatile u32 g_ticks;

static const char *const exc_name[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection", "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD exception", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved"
};

void panic(const char *msg)
{
    cpu_cli();
    kprintf("\n*** KERNEL PANIC: %s\n", msg);
    panic_screen(msg);
    for (;;) cpu_hlt();
}

void isr_handler(Regs *r)
{
    if (r->int_no < 32) {
        char buf[160];
        ksnprintf(buf, sizeof buf, "%s (vector %u, error %x) at EIP %x", exc_name[r->int_no],
                  r->int_no, r->err, r->eip);
        kprintf("\n*** EXCEPTION: %s\n    eax=%x ebx=%x ecx=%x edx=%x\n    esi=%x edi=%x ebp=%x esp=%x eflags=%x\n",
                buf, r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r->esp, r->eflags);
        panic_screen(buf);
        for (;;) { cpu_cli(); cpu_hlt(); }
    }

    int irq = (int)r->int_no - 32;
    if (irq == 7) {                             /* spurious IRQ on the master PIC */
        outb(0x20, 0x0B);
        if (!(inb(0x20) & 0x80)) return;
    }
    if (irq == 15) {                            /* spurious IRQ on the slave PIC */
        outb(0xA0, 0x0B);
        if (!(inb(0xA0) & 0x80)) { outb(0x20, 0x20); return; }
    }
    if (irq_fn[irq]) irq_fn[irq]();
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void irq_install(int irq, void (*fn)(void))
{
    irq_fn[irq] = fn;
    if (irq < 8) {
        outb(0x21, inb(0x21) & (u8)~(1 << irq));
    } else {
        outb(0xA1, inb(0xA1) & (u8)~(1 << (irq - 8)));
        outb(0x21, inb(0x21) & (u8)~(1 << 2));  /* cascade line */
    }
}

static void timer_irq(void) { g_ticks++; }

void cpu_init(void)
{
    for (int i = 0; i < 48; i++) {
        idt[i].off_lo = (u16)(isr_table[i] & 0xFFFF);
        idt[i].off_hi = (u16)(isr_table[i] >> 16);
        idt[i].sel = 0x08;
        idt[i].zero = 0;
        idt[i].flags = 0x8E;                    /* present, ring 0, 32-bit interrupt gate */
    }
    struct idt_ptr p = { sizeof idt - 1, (u32)(uptr)idt };
    __asm__ volatile("lidt %0" : : "m"(p));

    /* remap the PICs to vectors 32..47 and mask everything */
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF); outb(0xA1, 0xFF);

    /* PIT channel 0: 1000 Hz square wave */
    u32 div = 1193182 / 1000;
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)(div >> 8));
    irq_install(0, timer_irq);
}

u32 plat_ticks(void) { return g_ticks; }
