/*
 * net.h -- Minimal network stack (Ethernet, ARP, IPv4, ICMP).
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

/* EtherTypes. */
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806

/* IP protocols. */
#define IP_PROTO_ICMP   1

/* ICMP types. */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* Ethernet header. */
struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;    /* big-endian */
} __attribute__((packed));

/* ARP packet (Ethernet + IPv4). */
struct arp_pkt {
    uint16_t hw_type;      /* 0x0001 */
    uint16_t proto_type;   /* 0x0800 */
    uint8_t  hw_len;       /* 6 */
    uint8_t  proto_len;    /* 4 */
    uint16_t op;           /* 1=request, 2=reply */
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

/* IPv4 header (fixed 20 bytes, no options). */
struct ipv4_hdr {
    uint8_t  ver_ihl;      /* 0x45 */
    uint8_t  dscp;
    uint16_t total_len;    /* big-endian */
    uint16_t ident;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

/* ICMP header. */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t ident;
    uint16_t seq;
} __attribute__((packed));

/* Byte-swap helpers (network byte order = big-endian). */
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

/* Initialise the network stack with a static IP. */
void net_init(uint8_t ip[4]);

/* Process one incoming Ethernet frame. */
void net_rx(const void *frame, uint16_t len, void *ctx);

/* Send an ICMP echo request (ping).  Returns 0 if sent. */
int net_ping(uint8_t dst_ip[4]);

/* Poll for ping replies. Returns latency in ticks, or -1. */
int net_ping_check(void);

/* Print network status. */
void net_status(void);

#endif /* NET_H */
