/* NodyOS - the narrow interface between hardware and everything above it.
 * The kernel implements it on bare metal; tools/hostsim.c implements it on a
 * development machine so the GUI and the 3D engine can be exercised natively. */
#ifndef NODY_PLAT_H
#define NODY_PLAT_H

#include "lib/types.h"

typedef struct DateTime { u16 year; u8 month, day, hour, min, sec; } DateTime;

u32   plat_ticks(void);                         /* milliseconds since boot */
void  plat_puts(const char *s);                 /* debug console (COM1)    */
void  plat_now(DateTime *dt);                   /* real-time clock         */
void  plat_meminfo(u32 *total_kb, u32 *used_kb);
const char *plat_cpu(void);
void  plat_reboot(void);
void  plat_poweroff(void);

void *kmalloc(usize n);
void  kfree(void *p);

int   disk_read(u32 lba, u32 count, void *buf); /* 0 on success            */
int   disk_write(u32 lba, u32 count, const void *buf);
u32   disk_sectors(void);

#endif
