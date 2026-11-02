// pcsim-mkcard: builds an SD card image for the simulator, laid out the way a PC formats a real
// card (MBR, one FAT32 partition) and filled from host directories.

#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ff.h"
#include "diskio.h"
#include "walk.h"

static FILE *img;
static LBA_t img_sectors;
static int files_copied, dirs_made;

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv || !img ? STA_NOINIT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img)
        return RES_NOTRDY;
    if (fseeko(img, (off_t)sector * 512, SEEK_SET) || fread(buff, 512, count, img) != count)
        return RES_ERROR;
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img)
        return RES_NOTRDY;
    if (fseeko(img, (off_t)sector * 512, SEEK_SET) || fwrite(buff, 512, count, img) != count)
        return RES_ERROR;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || !img)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return fflush(img) ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = img_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

static DWORD fat_time(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80)
        return (DWORD)1 << 21 | (DWORD)1 << 16;
    return (DWORD)(tm.tm_year - 80) << 25 | (DWORD)(tm.tm_mon + 1) << 21 | (DWORD)tm.tm_mday << 16 |
           (DWORD)tm.tm_hour << 11 | (DWORD)tm.tm_min << 5 | (DWORD)(tm.tm_sec / 2);
}

DWORD get_fattime(void)
{
    return fat_time(time(NULL));
}

static void set_mtime(const char *fat_path, time_t mtime)
{
    FILINFO fno;
    DWORD t = fat_time(mtime);
    fno.fdate = (WORD)(t >> 16);
    fno.ftime = (WORD)t;
    f_utime(fat_path, &fno);
}

static int copy_file(const char *host_path, const char *fat_path, time_t mtime)
{
    FILE *in = fopen(host_path, "rb");
    if (!in) {
        fprintf(stderr, "mkcard: %s: %s\n", host_path, strerror(errno));
        return -1;
    }
    FIL out;
    FRESULT fr = f_open(&out, fat_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        fprintf(stderr, "mkcard: cannot create %s on the card (FatFs error %d)\n", fat_path, fr);
        fclose(in);
        return -1;
    }
    char buf[32768];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        UINT written;
        fr = f_write(&out, buf, (UINT)n, &written);
        if (fr != FR_OK || written != n) {
            fprintf(stderr, "mkcard: writing %s failed (card full?)\n", fat_path);
            rc = -1;
            break;
        }
    }
    f_close(&out);
    fclose(in);
    if (rc == 0) {
        set_mtime(fat_path, mtime);
        files_copied++;
    }
    return rc;
}

static int copy_entry(void *ctx, walk_event_t event, const char *host_path, const char *fat_path, time_t mtime)
{
    (void)ctx;
    switch (event) {
    case WALK_FILE:
        return copy_file(host_path, fat_path, mtime);
    case WALK_DIR_ENTER: {
        FRESULT fr = f_mkdir(fat_path);
        if (fr != FR_OK && fr != FR_EXIST) {
            fprintf(stderr, "mkcard: cannot make %s (FatFs error %d)\n", fat_path, fr);
            return -1;
        }
        dirs_made++;
        return 0;
    }
    case WALK_DIR_LEAVE:
        set_mtime(fat_path, mtime);
        return 0;
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pcsim-mkcard IMAGE [-s MB] [-l LABEL] [DIR...]\n"
            "\n"
            "Creates IMAGE as an MBR-partitioned FAT32 card of MB megabytes (default 512)\n"
            "and copies the contents of each DIR into its root. An existing IMAGE is replaced.\n");
}

int main(int argc, char **argv)
{
    unsigned size_mb = 512;
    const char *label = "PICOCALC";
    int opt;
    while ((opt = getopt(argc, argv, "s:l:h")) != -1) {
        switch (opt) {
        case 's':
            size_mb = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'l':
            label = optarg;
            break;
        default:
            usage();
            return opt == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage();
        return 2;
    }
    // FAT32 needs at least 65525 clusters, which FatFs cannot fit below about 64 MB.
    if (size_mb < 64) {
        fprintf(stderr, "mkcard: FAT32 needs at least 64 MB\n");
        return 2;
    }
    const char *path = argv[optind];

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    img = fopen(tmp, "w+b");
    if (!img) {
        fprintf(stderr, "mkcard: %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    img_sectors = (LBA_t)size_mb * 2048;
    if (ftruncate(fileno(img), (off_t)img_sectors * 512) != 0) {
        fprintf(stderr, "mkcard: cannot size %s: %s\n", tmp, strerror(errno));
        return 1;
    }

    static BYTE work[FF_MAX_SS * 64];
    MKFS_PARM parm = {FM_FAT32, 2, 0, 0, 0};
    FRESULT fr = f_mkfs("", &parm, work, sizeof(work));
    if (fr != FR_OK) {
        fprintf(stderr, "mkcard: formatting failed (FatFs error %d)\n", fr);
        return 1;
    }
    FATFS fs;
    if ((fr = f_mount(&fs, "", 1)) != FR_OK) {
        fprintf(stderr, "mkcard: cannot mount the new volume (FatFs error %d)\n", fr);
        return 1;
    }
    f_setlabel(label);

    int rc = 0;
    for (int i = optind + 1; i < argc; i++)
        if (walk_tree(argv[i], "", copy_entry, NULL) != 0)
            rc = 1;

    f_unmount("");
    if (fclose(img) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "mkcard: cannot finish %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("%s: %u MB FAT32, %d files in %d directories\n", path, size_mb, files_copied, dirs_made);
    return rc;
}
