/* NodyFS - see fs.h for the on-disk layout. */
#include "lib/fs.h"
#include "lib/lib.h"
#include "lib/plat.h"

#define FS_LBA       2048
#define FS_SPB       (FS_BLOCK / 512)
#define FS_DIR_SECT  ((FS_MAX_FILES * 64) / 512)
#define FS_EOC       0xFFFFFFFFu
#define FS_VERSION   1

typedef struct FsSuper {
    char magic[8];              /* "NODYFS01" */
    u32  version;
    u32  total_blocks;          /* data blocks, including reserved block 0 */
    u32  dir_lba, dir_sectors;
    u32  fat_lba, fat_sectors;
    u32  data_lba;
    u32  reserved;
} FsSuper;

static FsSuper sb;
static FsEntry dir[FS_MAX_FILES];
static u32    *fat;
static bool    mounted, formatted;
static u8      sector[512];
static u8      block[FS_BLOCK];

static int write_super(void)
{
    memset(sector, 0, sizeof sector);
    memcpy(sector, &sb, sizeof sb);
    return disk_write(FS_LBA, 1, sector);
}

static int write_dir(void) { return disk_write(sb.dir_lba, sb.dir_sectors, dir); }
static int write_fat(void) { return disk_write(sb.fat_lba, sb.fat_sectors, fat); }

static bool format(u32 disk)
{
    if (disk < FS_LBA + 1 + FS_DIR_SECT + 64) return false;
    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, "NODYFS01", 8);
    sb.version = FS_VERSION;
    sb.dir_lba = FS_LBA + 1;
    sb.dir_sectors = FS_DIR_SECT;
    sb.fat_lba = sb.dir_lba + sb.dir_sectors;
    u32 rest = disk - sb.fat_lba;
    u32 blocks = rest / FS_SPB;                         /* upper bound */
    sb.fat_sectors = (blocks * 4 + 511) / 512;
    sb.data_lba = sb.fat_lba + sb.fat_sectors;
    sb.total_blocks = (disk - sb.data_lba) / FS_SPB;

    fat = kmalloc(sb.fat_sectors * 512);
    if (!fat) return false;
    memset(fat, 0, sb.fat_sectors * 512);
    fat[0] = FS_EOC;
    memset(dir, 0, sizeof dir);
    if (write_dir() || write_fat() || write_super()) return false;
    formatted = true;
    return true;
}

bool fs_mount(void)
{
    u32 disk = disk_sectors();
    mounted = false;
    if (!disk || disk_read(FS_LBA, 1, sector)) return false;
    memcpy(&sb, sector, sizeof sb);
    if (memcmp(sb.magic, "NODYFS01", 8) != 0 || sb.version != FS_VERSION ||
        sb.data_lba + sb.total_blocks * FS_SPB > disk || sb.fat_sectors * 128 < sb.total_blocks) {
        if (!format(disk)) return false;
    } else {
        fat = kmalloc(sb.fat_sectors * 512);
        if (!fat) return false;
        if (disk_read(sb.dir_lba, sb.dir_sectors, dir)) return false;
        if (disk_read(sb.fat_lba, sb.fat_sectors, fat)) return false;
    }
    mounted = true;
    return true;
}

bool fs_mounted(void) { return mounted; }
bool fs_was_formatted(void) { return formatted; }

static int find(const char *name)
{
    for (int i = 0; i < FS_MAX_FILES; i++)
        if ((dir[i].flags & 1) && str_ncmp(dir[i].name, name, FS_NAME_MAX) == 0) return i;
    return -1;
}

static bool valid_name(const char *name)
{
    usize n = str_len(name);
    if (n == 0 || n >= FS_NAME_MAX) return false;
    for (usize i = 0; i < n; i++)
        if (name[i] <= 32 || name[i] > 126 || name[i] == '/') return false;
    return true;
}

int fs_list(FsEntry *out, int max)
{
    int n = 0;
    if (!mounted) return 0;
    for (int i = 0; i < FS_MAX_FILES && n < max; i++)
        if (dir[i].flags & 1) out[n++] = dir[i];
    for (int i = 1; i < n; i++) {                       /* insertion sort by name */
        FsEntry e = out[i];
        int j = i;
        while (j > 0 && str_cmp(out[j - 1].name, e.name) > 0) { out[j] = out[j - 1]; j--; }
        out[j] = e;
    }
    return n;
}

int fs_size(const char *name)
{
    int i = mounted ? find(name) : -1;
    return i < 0 ? -1 : (int)dir[i].size;
}

int fs_read(const char *name, void *buf, u32 max)
{
    if (!mounted) return -1;
    int i = find(name);
    if (i < 0) return -1;
    u32 left = dir[i].size < max ? dir[i].size : max;
    u32 total = left, b = dir[i].first;
    u8 *out = buf;
    while (left > 0) {
        if (b == 0 || b == FS_EOC || b >= sb.total_blocks) return -1;
        if (disk_read(sb.data_lba + b * FS_SPB, FS_SPB, block)) return -1;
        u32 n = left < FS_BLOCK ? left : FS_BLOCK;
        memcpy(out, block, n);
        out += n;
        left -= n;
        b = fat[b];
    }
    return (int)total;
}

static void free_chain(u32 b)
{
    while (b != 0 && b != FS_EOC && b < sb.total_blocks) {
        u32 next = fat[b];
        fat[b] = 0;
        b = next;
    }
}

static u32 pack_now(void)
{
    DateTime t;
    plat_now(&t);
    u32 y = t.year >= 2000 ? (u32)(t.year - 2000) : 0;
    return (y << 26) | ((u32)t.month << 22) | ((u32)t.day << 17) | ((u32)t.hour << 12) | ((u32)t.min << 6) | t.sec;
}

void fs_unpack_time(u32 t, int *year, int *month, int *day, int *hour, int *min)
{
    *year = 2000 + (int)(t >> 26);
    *month = (int)((t >> 22) & 15);
    *day = (int)((t >> 17) & 31);
    *hour = (int)((t >> 12) & 31);
    *min = (int)((t >> 6) & 63);
}

/* Order of updates: new data blocks, then the allocation table, then the
 * directory. An interrupted save therefore leaves the old file intact. */
int fs_write(const char *name, const void *buf, u32 size)
{
    if (!mounted) return -1;
    if (!valid_name(name)) return -2;
    int slot = find(name);
    if (slot < 0) {
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (!(dir[i].flags & 1)) { slot = i; break; }
        if (slot < 0) return -3;                        /* directory full */
        memset(&dir[slot], 0, sizeof dir[slot]);
    }

    u32 need = (size + FS_BLOCK - 1) / FS_BLOCK;
    u32 first = 0, prev = 0, scan = 1;
    const u8 *in = buf;
    u32 left = size;
    for (u32 k = 0; k < need; k++) {
        while (scan < sb.total_blocks && fat[scan] != 0) scan++;
        if (scan >= sb.total_blocks) { free_chain(first); return -4; }   /* disk full */
        u32 b = scan;
        fat[b] = FS_EOC;
        if (prev) fat[prev] = b; else first = b;
        prev = b;

        u32 n = left < FS_BLOCK ? left : FS_BLOCK;
        memset(block, 0, FS_BLOCK);
        memcpy(block, in, n);
        if (disk_write(sb.data_lba + b * FS_SPB, FS_SPB, block)) { free_chain(first); return -5; }
        in += n;
        left -= n;
    }

    u32 old = (dir[slot].flags & 1) ? dir[slot].first : 0;
    free_chain(old);
    str_copy(dir[slot].name, name, FS_NAME_MAX);
    dir[slot].size = size;
    dir[slot].first = first;
    dir[slot].flags = 1;
    dir[slot].mtime = pack_now();
    if (write_fat() || write_dir()) return -5;
    return 0;
}

int fs_delete(const char *name)
{
    if (!mounted) return -1;
    int i = find(name);
    if (i < 0) return -1;
    free_chain(dir[i].first);
    memset(&dir[i], 0, sizeof dir[i]);
    if (write_fat() || write_dir()) return -5;
    return 0;
}

u32 fs_free_bytes(void)
{
    u32 n = 0;
    if (!mounted) return 0;
    for (u32 b = 1; b < sb.total_blocks; b++)
        if (fat[b] == 0) n++;
    return n * FS_BLOCK;
}

u32 fs_total_bytes(void)
{
    return mounted ? (sb.total_blocks - 1) * FS_BLOCK : 0;
}
