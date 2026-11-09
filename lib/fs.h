/* NodyFS - the NodyOS on-disk filesystem.
 *
 * A flat volume starting at sector 2048 of the boot disk:
 *
 *   sector 0        superblock
 *   sectors 1..8    directory: 64 entries of 64 bytes
 *   sectors 9..     block allocation table: one u32 per 4 KiB data block
 *                   (0 = free, 0xFFFFFFFF = end of chain, else next block)
 *   then            data blocks
 *
 * Files are chains of 4 KiB blocks. Block 0 is reserved so that 0 can mean
 * "free" in the table and "no data" in a directory entry.
 */
#ifndef NODY_FS_H
#define NODY_FS_H

#include "lib/types.h"

#define FS_NAME_MAX  32         /* including the terminating zero */
#define FS_MAX_FILES 64
#define FS_BLOCK     4096

typedef struct FsEntry {
    char name[FS_NAME_MAX];
    u32  size;
    u32  first;                 /* first data block, 0 if empty */
    u32  flags;                 /* bit 0: in use */
    u32  mtime;                 /* packed date, see fs_unpack_time */
    u32  reserved[4];
} FsEntry;                      /* 64 bytes on disk */

bool fs_mount(void);                    /* formats the volume if none is found */
bool fs_mounted(void);
bool fs_was_formatted(void);            /* true if this boot created the volume */
int  fs_list(FsEntry *out, int max);    /* sorted by name, returns count */
int  fs_size(const char *name);         /* -1 if missing */
int  fs_read(const char *name, void *buf, u32 max);     /* bytes read or -1 */
int  fs_write(const char *name, const void *buf, u32 size); /* 0 or negative */
int  fs_delete(const char *name);
u32  fs_free_bytes(void);
u32  fs_total_bytes(void);
void fs_unpack_time(u32 t, int *year, int *month, int *day, int *hour, int *min);

#endif
