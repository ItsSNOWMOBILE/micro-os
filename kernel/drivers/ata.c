/*
 * ata.c -- ATA PIO block device driver.
 *
 * Supports 28-bit LBA reads on the primary ATA channel using PIO
 * (programmed I/O).  No DMA, no IRQ — polling only.  This is the
 * simplest possible block device interface for reading disk images.
 */

#include "ata.h"
#include "../kernel.h"
#include "../console.h"

static bool drive_present[2];  /* master, slave */

/* ── Low-level helpers ─────────────────────────────────────────────────── */

static void
ata_wait_bsy(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(ATA_PRIMARY_IO + ATA_REG_STATUS) & ATA_SR_BSY))
            return;
    }
}

static int
ata_poll(void)
{
    /* Read status 4 times to give the drive 400ns delay. */
    for (int i = 0; i < 4; i++)
        inb(ATA_PRIMARY_IO + ATA_REG_STATUS);

    ata_wait_bsy();

    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status & ATA_SR_ERR)
        return -1;
    if (!(status & ATA_SR_DRQ))
        return -1;
    return 0;
}

/* ── IDENTIFY ──────────────────────────────────────────────────────────── */

static bool
ata_identify(int drive)
{
    uint8_t drv = (drive == 0) ? ATA_DRIVE_MASTER : ATA_DRIVE_SLAVE;

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, drv);
    /* Wait after drive select. */
    for (int i = 0; i < 4; i++)
        inb(ATA_PRIMARY_IO + ATA_REG_STATUS);

    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LO, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HI, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0)
        return false;  /* no drive */

    ata_wait_bsy();

    /* Check for ATAPI/SATA — LBA mid/hi become non-zero. */
    if (inb(ATA_PRIMARY_IO + ATA_REG_LBA_MID) != 0 ||
        inb(ATA_PRIMARY_IO + ATA_REG_LBA_HI) != 0)
        return false;  /* not ATA */

    /* Wait for DRQ or ERR with timeout. */
    for (int i = 0; i < 100000; i++) {
        status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)
            return false;
        if (status & ATA_SR_DRQ)
            break;
        if (i == 99999)
            return false;
    }

    /* Read and discard 256 words of identify data. */
    for (int i = 0; i < 256; i++)
        inw(ATA_PRIMARY_IO + ATA_REG_DATA);

    return true;
}

/* ── Init ──────────────────────────────────────────────────────────────── */

void
ata_init(void)
{
    /* Check if primary ATA controller exists (floating bus reads 0xFF). */
    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("ATA: no primary controller\n");
        return;
    }

    /* Software reset the primary channel. */
    outb(ATA_PRIMARY_CTRL, 0x04);
    for (int i = 0; i < 4; i++) inb(ATA_PRIMARY_CTRL);
    outb(ATA_PRIMARY_CTRL, 0x00);

    /* Wait for BSY to clear with a timeout. */
    for (int i = 0; i < 100000; i++) {
        if (!(inb(ATA_PRIMARY_IO + ATA_REG_STATUS) & ATA_SR_BSY))
            break;
    }

    drive_present[0] = ata_identify(0);
    drive_present[1] = ata_identify(1);

    if (drive_present[0])
        kprintf("ATA: master drive detected\n");
    if (drive_present[1])
        kprintf("ATA: slave drive detected\n");
    if (!drive_present[0] && !drive_present[1])
        kprintf("ATA: no drives detected\n");
}

bool
ata_drive_present(int drive)
{
    if (drive < 0 || drive > 1) return false;
    return drive_present[drive];
}

/* ── Read sectors ──────────────────────────────────────────────────────── */

int
ata_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf)
{
    if (drive < 0 || drive > 1 || !drive_present[drive])
        return -1;

    uint8_t drv = (drive == 0) ? ATA_DRIVE_MASTER : ATA_DRIVE_SLAVE;

    ata_wait_bsy();

    /* Select drive + top 4 bits of LBA. */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE,
         drv | ((lba >> 24) & 0x0F));

    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LO,  (uint8_t)(lba));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    uint16_t *p = (uint16_t *)buf;
    int sectors = (count == 0) ? 256 : count;

    for (int s = 0; s < sectors; s++) {
        if (ata_poll() < 0)
            return -1;
        for (int i = 0; i < 256; i++)
            *p++ = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    }

    return 0;
}
