/*
 * net.c -- Minimal network stack: Ethernet, ARP, IPv4, ICMP.
 *
 * Provides basic ping (ICMP echo request/reply) over a virtio-net device.
 * ARP is handled inline: requests are answered, and outgoing packets
 * trigger ARP resolution with a simple blocking wait.
 */

#include "net.h"
#include "../drivers/virtio_net.h"
#include "../console.h"
#include "../string.h"
#include "../drivers/timer.h"

/* Our network identity. */
static uint8_t our_mac[6];
static uint8_t our_ip[4];
static bool    net_up;

/* ── ARP cache (tiny, linear scan) ───────────────────────────────────────── */

#define ARP_CACHE_SIZE 16

struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    bool    valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static struct arp_entry *
arp_lookup(const uint8_t ip[4])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0)
            return &arp_cache[i];
    return NULL;
}

static void
arp_insert(const uint8_t ip[4], const uint8_t mac[6])
{
    /* Update existing. */
    struct arp_entry *e = arp_lookup(ip);
    if (e) {
        memcpy(e->mac, mac, 6);
        return;
    }
    /* Find free slot. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = true;
            return;
        }
    }
    /* Cache full: overwrite slot 0. */
    memcpy(arp_cache[0].ip, ip, 4);
    memcpy(arp_cache[0].mac, mac, 6);
}

/* ── Checksum ────────────────────────────────────────────────────────────── */

static uint16_t
ip_checksum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const uint8_t *)p;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Frame transmit helpers ──────────────────────────────────────────────── */

static int
send_frame(const uint8_t dst_mac[6], uint16_t ethertype,
           const void *payload, uint16_t payload_len)
{
    uint8_t frame[1518];
    if ((uint16_t)(14 + payload_len) > (uint16_t)sizeof(frame)) return -1;

    struct eth_hdr *eth = (struct eth_hdr *)frame;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons(ethertype);
    memcpy(frame + 14, payload, payload_len);

    return virtio_net_tx(frame, 14 + payload_len);
}

/* ── ARP ─────────────────────────────────────────────────────────────────── */

static void
arp_send_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4])
{
    struct arp_pkt arp;
    arp.hw_type    = htons(0x0001);
    arp.proto_type = htons(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.op         = htons(2);  /* reply */
    memcpy(arp.sender_mac, our_mac, 6);
    memcpy(arp.sender_ip,  our_ip,  4);
    memcpy(arp.target_mac, dst_mac, 6);
    memcpy(arp.target_ip,  dst_ip,  4);
    send_frame(dst_mac, ETHERTYPE_ARP, &arp, sizeof(arp));
}

static void
arp_send_request(const uint8_t target_ip[4])
{
    struct arp_pkt arp;
    arp.hw_type    = htons(0x0001);
    arp.proto_type = htons(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.op         = htons(1);  /* request */
    memcpy(arp.sender_mac, our_mac, 6);
    memcpy(arp.sender_ip,  our_ip,  4);
    memset(arp.target_mac, 0xFF, 6);
    memcpy(arp.target_ip,  target_ip, 4);

    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    send_frame(bcast, ETHERTYPE_ARP, &arp, sizeof(arp));
}

static void
handle_arp(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *arp = (const struct arp_pkt *)data;

    /* Learn sender's MAC. */
    arp_insert(arp->sender_ip, arp->sender_mac);

    uint16_t op = ntohs(arp->op);
    if (op == 1) {
        /* ARP request: is it asking for us? */
        if (memcmp(arp->target_ip, our_ip, 4) == 0)
            arp_send_reply(arp->sender_mac, arp->sender_ip);
    }
    /* For replies (op==2), we already cached the sender above. */
}

/* ── Resolve IP → MAC (blocking, with timeout) ──────────────────────────── */

static int
arp_resolve(const uint8_t ip[4], uint8_t mac_out[6])
{
    /* Broadcast? */
    static const uint8_t bcast_ip[4] = {255,255,255,255};
    if (memcmp(ip, bcast_ip, 4) == 0) {
        memset(mac_out, 0xFF, 6);
        return 0;
    }

    struct arp_entry *e = arp_lookup(ip);
    if (e) {
        memcpy(mac_out, e->mac, 6);
        return 0;
    }

    /* Send ARP request, poll for reply with retries. */
    for (int retry = 0; retry < 3; retry++) {
        arp_send_request(ip);
        uint64_t deadline = timer_ticks() + 100;  /* 1 second per attempt */
        while (timer_ticks() < deadline) {
            virtio_net_rx_poll(net_rx, NULL);
            e = arp_lookup(ip);
            if (e) {
                memcpy(mac_out, e->mac, 6);
                return 0;
            }
            /* Brief yield so timer interrupts can fire. */
            __asm__ volatile("hlt");
        }
    }
    return -1;  /* timeout */
}

/* ── IPv4 ────────────────────────────────────────────────────────────────── */

static int
send_ipv4(const uint8_t dst_ip[4], uint8_t protocol,
          const void *payload, uint16_t payload_len)
{
    uint8_t mac[6];
    if (arp_resolve(dst_ip, mac) < 0)
        return -1;

    uint8_t pkt[1500];
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    ip->ver_ihl    = 0x45;
    ip->dscp       = 0;
    ip->total_len  = htons(20 + payload_len);
    ip->ident      = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = protocol;
    ip->checksum   = 0;
    memcpy(ip->src, our_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum = ip_checksum(ip, 20);

    memcpy(pkt + 20, payload, payload_len);

    return send_frame(mac, ETHERTYPE_IPV4, pkt, 20 + payload_len);
}

static void
handle_ipv4(const uint8_t *data, uint16_t len);

/* ── ICMP ────────────────────────────────────────────────────────────────── */

/* Ping state. */
static bool     ping_pending;
static uint64_t ping_sent_tick;
static uint16_t ping_seq;
static int      ping_result;  /* latency in ticks, or -1 */

static void
handle_icmp(const uint8_t *ip_data, uint16_t ip_len,
            const struct ipv4_hdr *ip)
{
    if (ip_len < 20 + 8) return;
    const struct icmp_hdr *icmp = (const struct icmp_hdr *)(ip_data + 20);
    uint16_t icmp_len = ip_len - 20;

    /* Verify ICMP checksum. */
    if (ip_checksum(icmp, icmp_len) != 0) return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        /* Send echo reply. */
        uint8_t reply[1500 - 20];
        if (icmp_len > sizeof(reply)) return;
        memcpy(reply, icmp, icmp_len);
        struct icmp_hdr *r = (struct icmp_hdr *)reply;
        r->type     = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = ip_checksum(r, icmp_len);
        send_ipv4(ip->src, IP_PROTO_ICMP, reply, icmp_len);

    } else if (icmp->type == ICMP_ECHO_REPLY && ping_pending) {
        if (ntohs(icmp->seq) == ping_seq) {
            ping_result = (int)(timer_ticks() - ping_sent_tick);
            ping_pending = false;
        }
    }
}

static void
handle_ipv4(const uint8_t *data, uint16_t len)
{
    if (len < 20) return;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)data;

    /* Only accept packets for us (or broadcast). */
    static const uint8_t bcast[4] = {255,255,255,255};
    if (memcmp(ip->dst, our_ip, 4) != 0 &&
        memcmp(ip->dst, bcast, 4) != 0)
        return;

    /* Verify header checksum. */
    if (ip_checksum(ip, 20) != 0) return;

    if (ip->protocol == IP_PROTO_ICMP) {
        uint16_t ip_len = ntohs(ip->total_len);
        if (ip_len > len) ip_len = len;
        handle_icmp(data, ip_len, ip);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
net_rx(const void *frame, uint16_t len, void *ctx)
{
    (void)ctx;
    if (len < 14) return;
    const struct eth_hdr *eth = (const struct eth_hdr *)frame;
    const uint8_t *payload = (const uint8_t *)frame + 14;
    uint16_t plen = len - 14;

    uint16_t etype = ntohs(eth->ethertype);
    if (etype == ETHERTYPE_ARP)
        handle_arp(payload, plen);
    else if (etype == ETHERTYPE_IPV4)
        handle_ipv4(payload, plen);
}

void
net_init(uint8_t ip[4])
{
    memcpy(our_ip, ip, 4);
    virtio_net_get_mac(our_mac);
    memset(arp_cache, 0, sizeof(arp_cache));
    ping_pending = false;
    ping_seq = 0;
    net_up = true;
    kprintf("[ok] Net: %d.%d.%d.%d\n",
            our_ip[0], our_ip[1], our_ip[2], our_ip[3]);
}

int
net_ping(uint8_t dst_ip[4])
{
    if (!net_up) return -1;

    ping_seq++;
    ping_pending = true;
    ping_result  = -1;
    ping_sent_tick = timer_ticks();

    struct icmp_hdr icmp;
    icmp.type     = ICMP_ECHO_REQUEST;
    icmp.code     = 0;
    icmp.checksum = 0;
    icmp.ident    = htons(0x1234);
    icmp.seq      = htons(ping_seq);
    icmp.checksum = ip_checksum(&icmp, sizeof(icmp));

    return send_ipv4(dst_ip, IP_PROTO_ICMP, &icmp, sizeof(icmp));
}

int
net_ping_check(void)
{
    if (!net_up) return -1;

    /* Poll for a bit. */
    uint64_t deadline = timer_ticks() + 300;  /* 3 seconds */
    while (ping_pending && timer_ticks() < deadline)
        virtio_net_rx_poll(net_rx, NULL);

    return ping_result;
}

void
net_status(void)
{
    if (!net_up) {
        kprintf("Network: down\n");
        return;
    }
    kprintf("Network: up\n");
    kprintf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            our_mac[0], our_mac[1], our_mac[2],
            our_mac[3], our_mac[4], our_mac[5]);
    kprintf("  IP:  %d.%d.%d.%d\n",
            our_ip[0], our_ip[1], our_ip[2], our_ip[3]);
    kprintf("  ARP cache:\n");
    int arp_count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            kprintf("    %d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                    arp_cache[i].ip[0], arp_cache[i].ip[1],
                    arp_cache[i].ip[2], arp_cache[i].ip[3],
                    arp_cache[i].mac[0], arp_cache[i].mac[1],
                    arp_cache[i].mac[2], arp_cache[i].mac[3],
                    arp_cache[i].mac[4], arp_cache[i].mac[5]);
            arp_count++;
        }
    }
    if (arp_count == 0)
        kprintf("    (empty)\n");
}
