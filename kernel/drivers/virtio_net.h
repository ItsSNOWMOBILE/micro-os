/*
 * virtio_net.h -- Virtio network device driver (legacy transport).
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stdbool.h>

/* Legacy virtio register offsets from BAR0. */
#define VIRTIO_DEV_FEATURES   0x00
#define VIRTIO_GUEST_FEATURES 0x04
#define VIRTIO_QUEUE_ADDR     0x08
#define VIRTIO_QUEUE_SIZE     0x0C
#define VIRTIO_QUEUE_SEL      0x0E
#define VIRTIO_QUEUE_NOTIFY   0x10
#define VIRTIO_DEV_STATUS     0x12
#define VIRTIO_ISR_STATUS     0x13
#define VIRTIO_NET_MAC        0x14

/* Device status bits. */
#define VIRTIO_STATUS_ACK        0x01
#define VIRTIO_STATUS_DRIVER     0x02
#define VIRTIO_STATUS_DRIVER_OK  0x04
#define VIRTIO_STATUS_FEATURES   0x08

/* Descriptor flags. */
#define VIRTQ_DESC_F_NEXT   0x01
#define VIRTQ_DESC_F_WRITE  0x02

/* Virtqueue structures. */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

/* Virtio-net packet header (10 bytes). */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE  10
#define VIRTQ_SIZE           128   /* queue depth (keep small) */

/* Initialise the virtio-net device. Returns 0 on success. */
int virtio_net_init(void);

/* Returns true if a virtio-net device was found and initialised. */
bool virtio_net_available(void);

/* Get the MAC address (6 bytes). */
void virtio_net_get_mac(uint8_t mac[6]);

/* Transmit a raw Ethernet frame (without virtio-net header).
 * Returns 0 on success. */
int virtio_net_tx(const void *frame, uint16_t len);

/* Poll for received packets. Calls cb(frame, len, ctx) for each.
 * Returns number of packets processed. */
int virtio_net_rx_poll(void (*cb)(const void *frame, uint16_t len, void *ctx),
                       void *ctx);

#endif /* VIRTIO_NET_H */
