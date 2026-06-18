#pragma once
#include <stdint.h>

/* ===========================================================================
 *  ATA / IDE PIO block driver  (drivers/ata.c)
 *
 *  Talks to real fixed disks over the legacy ATA register file (ports
 *  0x1F0/0x3F6 primary, 0x170/0x376 secondary). The same command set drives
 *  both spinning HDDs and SSDs - the only difference is the media type, which
 *  we read out of the IDENTIFY page (word 217, "nominal media rotation rate":
 *  1 == non-rotating == SSD, anything else == HDD). Polled PIO, no IRQs, no
 *  DMA: simple and correct under QEMU and on plain PATA/SATA-in-IDE-mode HW.
 * ===========================================================================*/

#define ATA_SECTOR 512u

typedef struct ata_dev {
    int       present;
    uint16_t  io_base;     /* command block base (0x1F0 / 0x170)            */
    uint16_t  ctrl_base;   /* control block base (0x3F6 / 0x376)           */
    uint8_t   slave;       /* 0 = master, 1 = slave                         */
    int       lba48;       /* 48-bit addressing supported                   */
    int       is_ssd;      /* 1 = SSD (rotation rate 1), 0 = HDD/unknown    */
    uint64_t  sectors;     /* total addressable 512-byte sectors            */
    char      model[41];   /* IDENTIFY model string, trimmed + NUL-term     */
} ata_dev;

/* Probe all four legacy ATA slots and fill the device table. Idempotent. */
void      ata_init(void);

/* Present-disk accessors (index 0..ata_count()-1, probe order). */
int       ata_count(void);
ata_dev  *ata_get(int i);

/* The disk the filesystem should persist to: the first present ATA disk that
 * is NOT the boot disk (primary master). NULL if none -> FS stays RAM-only. */
ata_dev  *ata_fs_disk(void);

/* Sector I/O. lba/count in 512-byte sectors. Return 0 on success, -1 on error
 * (absent disk, out-of-range LBA, or controller error/timeout). */
int       ata_read (ata_dev *d, uint64_t lba, uint32_t count, void *buf);
int       ata_write(ata_dev *d, uint64_t lba, uint32_t count, const void *buf);

const char *ata_media(const ata_dev *d);   /* "SSD" / "HDD" */
