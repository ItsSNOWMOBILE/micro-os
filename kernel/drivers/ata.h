/*
 * ata.h -- ATA PIO block device driver.
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

/* ATA primary channel I/O ports. */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

/* ATA registers (offsets from base). */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LO      0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HI      0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07

/* ATA status bits. */
#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

/* ATA commands. */
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_IDENTIFY     0xEC

/* Drive select values. */
#define ATA_DRIVE_MASTER     0xE0
#define ATA_DRIVE_SLAVE      0xF0

/* Initialise ATA driver, detect drives. */
void ata_init(void);

/* Returns true if the specified drive exists (0 = master, 1 = slave). */
bool ata_drive_present(int drive);

/* Read sectors via PIO. Returns 0 on success.
 * drive: 0 = master, 1 = slave
 * lba: starting sector (28-bit LBA)
 * count: number of sectors (1-256, 0 means 256)
 * buf: destination buffer (must hold count * 512 bytes) */
int ata_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf);

#endif /* ATA_H */
