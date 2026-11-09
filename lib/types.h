/* NodyOS - basic types. No libc: everything is defined here. */
#ifndef NODY_TYPES_H
#define NODY_TYPES_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long long          i64;
typedef __SIZE_TYPE__      usize;
typedef __UINTPTR_TYPE__   uptr;
typedef __builtin_va_list  kva_list;

#ifndef __bool_true_false_are_defined
typedef _Bool bool;
#define true  1
#define false 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif
