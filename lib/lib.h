/* NodyOS - freestanding support library (memory, strings, formatting). */
#ifndef NODY_LIB_H
#define NODY_LIB_H

#include "lib/types.h"

void *memcpy(void *d, const void *s, usize n);
void *memmove(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
int   memcmp(const void *a, const void *b, usize n);
void  mem_set32(u32 *d, u32 v, usize n);

usize str_len(const char *s);
int   str_cmp(const char *a, const char *b);
int   str_ncmp(const char *a, const char *b, usize n);
void  str_copy(char *d, const char *s, usize cap);      /* always terminates */
void  str_cat(char *d, const char *s, usize cap);
bool  str_ends(const char *s, const char *suffix);

int   kvsnprintf(char *buf, usize cap, const char *fmt, kva_list ap);
int   ksnprintf(char *buf, usize cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* little-endian byte stream helpers (file formats) */
static inline void put_u16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void put_u32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline u32  get_u16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static inline u32  get_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline void put_f32(u8 *p, float f) { union { float f; u32 u; } c; c.f = f; put_u32(p, c.u); }
static inline float get_f32(const u8 *p) { union { float f; u32 u; } c; c.u = get_u32(p); return c.f; }

#endif
