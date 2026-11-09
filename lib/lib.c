/* NodyOS - freestanding support library. */
#include "lib/lib.h"
#include "lib/plat.h"

#ifndef NODY_HOST
/* The compiler may emit calls to these four even in freestanding mode, so they
 * are written in assembly and can never recurse into themselves.
 *
 * Bulk work goes through unaligned SSE moves, 64 bytes per iteration: measured
 * under QEMU's emulation this is about six times faster than "rep movs" /
 * "rep stos", and every screen update is made of large copies and fills.
 * Interrupt entry saves the full SSE state, so this is safe everywhere. */
void *memcpy(void *d, const void *s, usize n)
{
    void *r = d;
    usize blocks = n >> 6;
    if (blocks) {
        __asm__ volatile(
            "1:\n"
            "movups   (%1), %%xmm0\n movups 16(%1), %%xmm1\n"
            "movups 32(%1), %%xmm2\n movups 48(%1), %%xmm3\n"
            "movups %%xmm0,   (%0)\n movups %%xmm1, 16(%0)\n"
            "movups %%xmm2, 32(%0)\n movups %%xmm3, 48(%0)\n"
            "add $64, %1\n add $64, %0\n dec %2\n jnz 1b"
            : "+r"(d), "+r"(s), "+r"(blocks) : : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc");
        n &= 63;
    }
    usize w = n >> 2, b = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(w) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(b) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, usize n)
{
    u8 *dp = d;
    const u8 *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n - 1;
    sp += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
    return d;
}

/* store the 32-bit pattern v into n dwords */
void mem_set32(u32 *d, u32 v, usize n)
{
    usize blocks = n >> 4;
    if (blocks) {
        __asm__ volatile(
            "movd %2, %%xmm0\n pshufd $0, %%xmm0, %%xmm0\n"
            "1:\n"
            "movups %%xmm0,   (%0)\n movups %%xmm0, 16(%0)\n"
            "movups %%xmm0, 32(%0)\n movups %%xmm0, 48(%0)\n"
            "add $64, %0\n dec %1\n jnz 1b"
            : "+r"(d), "+r"(blocks) : "r"(v) : "xmm0", "memory", "cc");
        n &= 15;
    }
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

void *memset(void *d, int c, usize n)
{
    u32 v = (u8)c * 0x01010101u;
    usize w = n >> 2, b = n & 3;
    u8 *tail = (u8 *)d + (w << 2);
    mem_set32(d, v, w);
    __asm__ volatile("rep stosb" : "+D"(tail), "+c"(b) : "a"(v) : "memory");
    return d;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = a, *y = b;
    for (usize i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
#else
void mem_set32(u32 *d, u32 v, usize n)
{
    for (usize i = 0; i < n; i++) d[i] = v;
}
#endif

usize str_len(const char *s)
{
    usize n = 0;
    while (s[n]) n++;
    return n;
}

int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int str_ncmp(const char *a, const char *b, usize n)
{
    for (usize i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (int)(u8)a[i] - (int)(u8)b[i];
    }
    return 0;
}

void str_copy(char *d, const char *s, usize cap)
{
    usize i = 0;
    if (!cap) return;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

void str_cat(char *d, const char *s, usize cap)
{
    usize n = str_len(d);
    if (n < cap) str_copy(d + n, s, cap - n);
}

bool str_ends(const char *s, const char *suffix)
{
    usize a = str_len(s), b = str_len(suffix);
    return a >= b && str_cmp(s + a - b, suffix) == 0;
}

/* ---- formatting ---------------------------------------------------------- */

typedef struct { char *buf; usize cap, len; } Out;

static void out_ch(Out *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(Out *o, const char *s, int n, int width, bool left, char pad)
{
    if (!left) for (int i = n; i < width; i++) out_ch(o, pad);
    for (int i = 0; i < n; i++) out_ch(o, s[i]);
    if (left) for (int i = n; i < width; i++) out_ch(o, ' ');
}

static int fmt_uint(char *tmp, u32 v, u32 base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[12];
    int n = 0;
    do { rev[n++] = digits[v % base]; v /= base; } while (v);
    for (int i = 0; i < n; i++) tmp[i] = rev[n - 1 - i];
    return n;
}

static int fmt_float(char *tmp, double v, int prec)
{
    int n = 0;
    if (v < 0) { tmp[n++] = '-'; v = -v; }
    if (v > 4.0e9) { tmp[n++] = 'i'; tmp[n++] = 'n'; tmp[n++] = 'f'; return n; }
    if (prec > 6) prec = 6;
    u32 scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    u32 ip = (u32)v;
    u32 fp = (u32)((v - (double)ip) * (double)scale + 0.5);
    if (fp >= scale) { fp -= scale; ip++; }
    n += fmt_uint(tmp + n, ip, 10, false);
    if (prec > 0) {
        tmp[n++] = '.';
        char f[8];
        int fn = fmt_uint(f, fp, 10, false);
        for (int i = fn; i < prec; i++) tmp[n++] = '0';
        for (int i = 0; i < fn; i++) tmp[n++] = f[i];
    }
    return n;
}

int kvsnprintf(char *buf, usize cap, const char *fmt, kva_list ap)
{
    Out o = { buf, cap, 0 };
    char tmp[48];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;
        bool left = false;
        char pad = ' ';
        int width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int v = __builtin_va_arg(ap, int);
            int n = 0;
            u32 u = (u32)v;
            if (v < 0) { tmp[n++] = '-'; u = 0u - u; }
            n += fmt_uint(tmp + n, u, 10, false);
            out_pad(&o, tmp, n, width, left, pad);
            break;
        }
        case 'u': out_pad(&o, tmp, fmt_uint(tmp, __builtin_va_arg(ap, u32), 10, false), width, left, pad); break;
        case 'x': out_pad(&o, tmp, fmt_uint(tmp, __builtin_va_arg(ap, u32), 16, false), width, left, pad); break;
        case 'X': out_pad(&o, tmp, fmt_uint(tmp, __builtin_va_arg(ap, u32), 16, true), width, left, pad); break;
        case 'p': {
            tmp[0] = '0'; tmp[1] = 'x';
            int n = 2 + fmt_uint(tmp + 2, (u32)(uptr)__builtin_va_arg(ap, void *), 16, false);
            out_pad(&o, tmp, n, width, left, ' ');
            break;
        }
        case 'f': out_pad(&o, tmp, fmt_float(tmp, __builtin_va_arg(ap, double), prec < 0 ? 2 : prec), width, left, pad); break;
        case 'c': tmp[0] = (char)__builtin_va_arg(ap, int); out_pad(&o, tmp, 1, width, left, ' '); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = (int)str_len(s);
            if (prec >= 0 && n > prec) n = prec;
            out_pad(&o, s, n, width, left, ' ');
            break;
        }
        case '%': out_ch(&o, '%'); break;
        case 0: fmt--; break;
        default: out_ch(&o, '%'); out_ch(&o, *fmt); break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, usize cap, const char *fmt, ...)
{
    kva_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[256];
    kva_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    plat_puts(buf);
}
