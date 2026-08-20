// Phase 50: UDP sockets — arch-agnostic, same shape as tcp.c: the kernel
// registers an IP-layer transmit callback; incoming datagrams arrive through
// udp_receive_packet() from the timer-IRQ rx dispatcher.
// Phase 51: DHCP + DNS clients live here too — they are plain UDP users.
#include <stdint.h>
#include "udp.h"

void *memset(void *s, int c, unsigned long n);
void *memcpy(void *dest, const void *src, unsigned long n);
extern uint32_t get_kernel_tick(void);
extern uint32_t kernel_get_timer_hz(void);
extern void kernel_net_get_mac(uint8_t *out);

static uint16_t u_htons(uint16_t h) { return (uint16_t)((h >> 8) | (h << 8)); }
#define u_ntohs u_htons

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    // header + payload
    uint16_t checksum;  // 0 = not computed (legal for IPv4)
} __attribute__((packed)) UdpHeader;

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[UDP_MAX_PAYLOAD];
} UdpDatagram;

typedef struct {
    uint8_t     in_use;
    uint16_t    port;
    uint8_t     rx_head, rx_count;
    UdpDatagram rx[UDP_RX_QUEUE];
} UdpSocket;

static UdpSocket g_socks[UDP_MAX_SOCKETS];
static UdpIpOutputFn g_ip_output = 0;
static uint32_t g_local_ip = 0;
static uint16_t g_next_ephemeral = 50000;

void udp_init(void) {
    memset(g_socks, 0, sizeof(g_socks));
}

void udp_set_output(UdpIpOutputFn fn, uint32_t local_ip) {
    g_ip_output = fn;
    g_local_ip = local_ip;
}

void udp_set_local_ip(uint32_t ip) {
    g_local_ip = ip;
}

int udp_bind(uint16_t port) {
    if (port == 0) {
        port = g_next_ephemeral++;
        if (g_next_ephemeral < 50000) g_next_ephemeral = 50000;
    }
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (g_socks[i].in_use && g_socks[i].port == port) return -1; // taken
    }
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!g_socks[i].in_use) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].in_use = 1;
            g_socks[i].port = port;
            return i;
        }
    }
    return -1;
}

int udp_close(int sock) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) return -1;
    g_socks[sock].in_use = 0;
    return 0;
}

uint16_t udp_local_port(int sock) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS || !g_socks[sock].in_use) return 0;
    return g_socks[sock].port;
}

int udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void *buf, int len) {
    static uint8_t seg[sizeof(UdpHeader) + UDP_MAX_PAYLOAD];
    if (sock < 0 || sock >= UDP_MAX_SOCKETS || !g_socks[sock].in_use) return -1;
    if (!g_ip_output || len < 0 || len > UDP_MAX_PAYLOAD) return -1;
    UdpHeader *h = (UdpHeader *)seg;
    h->src_port = u_htons(g_socks[sock].port);
    h->dst_port = u_htons(dst_port);
    h->length   = u_htons((uint16_t)(sizeof(UdpHeader) + len));
    h->checksum = 0; // optional in IPv4
    memcpy(seg + sizeof(UdpHeader), buf, (unsigned long)len);
    // DHCP special case: before we have an address, source must be 0.0.0.0
    uint32_t src = (dst_ip == 0xFFFFFFFFu && g_socks[sock].port == 68) ? 0 : g_local_ip;
    return g_ip_output(src, dst_ip, seg, (int)sizeof(UdpHeader) + len);
}

int udp_recvfrom(int sock, void *buf, int maxlen, uint32_t *src_ip, uint16_t *src_port) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS || !g_socks[sock].in_use) return -1;
    UdpSocket *s = &g_socks[sock];
    if (s->rx_count == 0) return 0;
    UdpDatagram *d = &s->rx[s->rx_head];
    int n = d->len < maxlen ? d->len : maxlen;
    memcpy(buf, d->data, (unsigned long)n);
    if (src_ip) *src_ip = d->src_ip;
    if (src_port) *src_port = d->src_port;
    s->rx_head = (uint8_t)((s->rx_head + 1) % UDP_RX_QUEUE);
    s->rx_count--;
    return n;
}

void udp_receive_packet(uint32_t src_ip, uint32_t dst_ip, const void *seg, int len) {
    (void)dst_ip; // we accept unicast + broadcast alike (DHCP needs both)
    if (len < (int)sizeof(UdpHeader)) return;
    const UdpHeader *h = (const UdpHeader *)seg;
    uint16_t dport = u_ntohs(h->dst_port);
    int plen = (int)u_ntohs(h->length) - (int)sizeof(UdpHeader);
    if (plen < 0 || plen > len - (int)sizeof(UdpHeader)) plen = len - (int)sizeof(UdpHeader);
    if (plen > UDP_MAX_PAYLOAD) plen = UDP_MAX_PAYLOAD;

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        UdpSocket *s = &g_socks[i];
        if (!s->in_use || s->port != dport) continue;
        if (s->rx_count >= UDP_RX_QUEUE) return; // queue full — drop
        UdpDatagram *d = &s->rx[(s->rx_head + s->rx_count) % UDP_RX_QUEUE];
        d->src_ip = src_ip;
        d->src_port = u_ntohs(h->src_port);
        d->len = (uint16_t)plen;
        memcpy(d->data, (const uint8_t *)seg + sizeof(UdpHeader), (unsigned long)plen);
        s->rx_count++;
        return;
    }
}

// ============================================================================
// Phase 51: DHCP client (RFC 2131, minimal DORA)
// ============================================================================

#define DHCP_MAGIC 0x63825363u

// Build a BOOTP/DHCP message. opts points at the option area to append to.
static int dhcp_build(uint8_t *pkt, uint32_t xid, const uint8_t *mac,
                      uint8_t msg_type, uint32_t req_ip, uint32_t server_ip) {
    memset(pkt, 0, 300);
    pkt[0] = 1;  // op = BOOTREQUEST
    pkt[1] = 1;  // htype = ethernet
    pkt[2] = 6;  // hlen
    pkt[4] = (uint8_t)(xid >> 24); pkt[5] = (uint8_t)(xid >> 16);
    pkt[6] = (uint8_t)(xid >> 8);  pkt[7] = (uint8_t)xid;
    pkt[10] = 0x80; // flags: broadcast reply
    memcpy(pkt + 28, mac, 6); // chaddr
    pkt[236] = 0x63; pkt[237] = 0x82; pkt[238] = 0x53; pkt[239] = 0x63; // magic
    int o = 240;
    pkt[o++] = 53; pkt[o++] = 1; pkt[o++] = msg_type; // DHCP message type
    if (req_ip) {
        pkt[o++] = 50; pkt[o++] = 4; // requested IP
        pkt[o++] = (uint8_t)(req_ip >> 24); pkt[o++] = (uint8_t)(req_ip >> 16);
        pkt[o++] = (uint8_t)(req_ip >> 8);  pkt[o++] = (uint8_t)req_ip;
    }
    if (server_ip) {
        pkt[o++] = 54; pkt[o++] = 4; // server identifier
        pkt[o++] = (uint8_t)(server_ip >> 24); pkt[o++] = (uint8_t)(server_ip >> 16);
        pkt[o++] = (uint8_t)(server_ip >> 8);  pkt[o++] = (uint8_t)server_ip;
    }
    pkt[o++] = 55; pkt[o++] = 3; // parameter request: mask, router, dns
    pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;
    pkt[o++] = 255; // end
    return o < 300 ? 300 : o;
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Parse a DHCP reply: returns message type (or 0), fills fields.
static uint8_t dhcp_parse(const uint8_t *pkt, int len, uint32_t xid,
                          uint32_t *yiaddr, uint32_t *mask, uint32_t *gw,
                          uint32_t *dns, uint32_t *server) {
    if (len < 244 || pkt[0] != 2) return 0;
    uint32_t rx_xid = rd_be32(pkt + 4);
    if (rx_xid != xid) return 0;
    if (rd_be32(pkt + 236) != DHCP_MAGIC) return 0;
    *yiaddr = rd_be32(pkt + 16);
    uint8_t msg_type = 0;
    int o = 240;
    while (o + 1 < len) {
        uint8_t opt = pkt[o];
        if (opt == 255) break;
        if (opt == 0) { o++; continue; }
        uint8_t olen = pkt[o + 1];
        if (o + 2 + olen > len) break;
        const uint8_t *v = pkt + o + 2;
        switch (opt) {
        case 53: if (olen >= 1) msg_type = v[0]; break;
        case 1:  if (olen >= 4) *mask = rd_be32(v); break;
        case 3:  if (olen >= 4) *gw = rd_be32(v); break;
        case 6:  if (olen >= 4) *dns = rd_be32(v); break;
        case 54: if (olen >= 4) *server = rd_be32(v); break;
        default: break;
        }
        o += 2 + olen;
    }
    return msg_type;
}

int dhcp_run(uint32_t *out_ip, uint32_t *out_mask, uint32_t *out_gw, uint32_t *out_dns) {
    uint8_t mac[6];
    kernel_net_get_mac(mac);
    uint32_t hz = kernel_get_timer_hz();
    if (hz == 0) hz = 240;

    int sock = udp_bind(68);
    if (sock < 0) return -1;

    static uint8_t pkt[UDP_MAX_PAYLOAD];
    uint32_t xid = (get_kernel_tick() << 8) ^ ((uint32_t)mac[5] << 4) ^ mac[4];
    int rc = -1;
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0, server = 0;

    for (int attempt = 0; attempt < 3 && rc != 0; attempt++) {
        // DISCOVER (broadcast)
        int n = dhcp_build(pkt, xid, mac, 1 /*DISCOVER*/, 0, 0);
        udp_sendto(sock, 0xFFFFFFFFu, 67, pkt, n);

        // Wait for OFFER
        uint32_t start = get_kernel_tick();
        int got_offer = 0;
        while (get_kernel_tick() - start < hz * 2) {
            uint32_t sip; uint16_t sport;
            int r = udp_recvfrom(sock, pkt, sizeof(pkt), &sip, &sport);
            if (r > 0 && dhcp_parse(pkt, r, xid, &ip, &mask, &gw, &dns, &server) == 2) {
                got_offer = 1;
                break;
            }
            __asm__ volatile("hlt");
        }
        if (!got_offer) continue;

        // REQUEST (broadcast, echoes offered IP + server id)
        n = dhcp_build(pkt, xid, mac, 3 /*REQUEST*/, ip, server);
        udp_sendto(sock, 0xFFFFFFFFu, 67, pkt, n);

        // Wait for ACK
        start = get_kernel_tick();
        while (get_kernel_tick() - start < hz * 2) {
            uint32_t sip; uint16_t sport;
            int r = udp_recvfrom(sock, pkt, sizeof(pkt), &sip, &sport);
            if (r > 0) {
                uint8_t t = dhcp_parse(pkt, r, xid, &ip, &mask, &gw, &dns, &server);
                if (t == 5) { rc = 0; break; }      // ACK
                if (t == 6) break;                   // NAK — retry
            }
            __asm__ volatile("hlt");
        }
    }

    udp_close(sock);
    if (rc == 0) {
        if (out_ip) *out_ip = ip;
        if (out_mask) *out_mask = mask;
        if (out_gw) *out_gw = gw;
        if (out_dns) *out_dns = dns;
    }
    return rc;
}

// ============================================================================
// Phase 51: DNS client (A records only)
// ============================================================================

static uint32_t g_dns_server = 0;

void dns_set_server(uint32_t ip) { g_dns_server = ip; }
uint32_t dns_get_server(void)   { return g_dns_server; }

// Skip a (possibly compressed) DNS name; returns new offset or -1.
static int dns_skip_name(const uint8_t *pkt, int len, int o) {
    while (o < len) {
        uint8_t l = pkt[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2; // compression pointer
        o += 1 + l;
    }
    return -1;
}

int dns_resolve(const char *name, uint32_t *out_ip) {
    if (!name || !name[0] || g_dns_server == 0) return -1;
    uint32_t hz = kernel_get_timer_hz();
    if (hz == 0) hz = 240;

    int sock = udp_bind(0);
    if (sock < 0) return -1;

    static uint8_t pkt[UDP_MAX_PAYLOAD];
    memset(pkt, 0, 12);
    uint16_t qid = (uint16_t)(get_kernel_tick() ^ 0x517A);
    pkt[0] = (uint8_t)(qid >> 8); pkt[1] = (uint8_t)qid;
    pkt[2] = 0x01; // RD
    pkt[5] = 1;    // QDCOUNT = 1
    int o = 12;

    // QNAME: dotted name -> length-prefixed labels
    int label_start = o++;
    int label_len = 0;
    for (const char *p = name; ; p++) {
        if (*p == '.' || *p == '\0') {
            if (label_len == 0 || label_len > 63) { udp_close(sock); return -1; }
            pkt[label_start] = (uint8_t)label_len;
            if (*p == '\0') break;
            label_start = o++;
            label_len = 0;
        } else {
            if (o >= 500) { udp_close(sock); return -1; }
            pkt[o++] = (uint8_t)*p;
            label_len++;
        }
    }
    pkt[o++] = 0;             // root
    pkt[o++] = 0; pkt[o++] = 1; // QTYPE  = A
    pkt[o++] = 0; pkt[o++] = 1; // QCLASS = IN

    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        udp_sendto(sock, g_dns_server, 53, pkt, o);
        uint32_t start = get_kernel_tick();
        while (get_kernel_tick() - start < hz * 2) {
            static uint8_t resp[UDP_MAX_PAYLOAD];
            uint32_t sip; uint16_t sport;
            int r = udp_recvfrom(sock, resp, sizeof(resp), &sip, &sport);
            if (r > 12 && resp[0] == (uint8_t)(qid >> 8) && resp[1] == (uint8_t)qid) {
                uint16_t qd = (uint16_t)((resp[4] << 8) | resp[5]);
                uint16_t an = (uint16_t)((resp[6] << 8) | resp[7]);
                int off = 12;
                for (int q = 0; q < qd && off > 0; q++) {
                    off = dns_skip_name(resp, r, off);
                    if (off > 0) off += 4; // qtype + qclass
                }
                for (int a = 0; a < an && off > 0 && off + 10 <= r; a++) {
                    off = dns_skip_name(resp, r, off);
                    if (off < 0 || off + 10 > r) break;
                    uint16_t type = (uint16_t)((resp[off] << 8) | resp[off + 1]);
                    uint16_t rdlen = (uint16_t)((resp[off + 8] << 8) | resp[off + 9]);
                    off += 10;
                    if (type == 1 && rdlen == 4 && off + 4 <= r) {
                        if (out_ip) *out_ip = rd_be32(resp + off);
                        rc = 0;
                        break;
                    }
                    off += rdlen;
                }
                break; // matching reply processed (found or not)
            }
            __asm__ volatile("hlt");
        }
    }
    udp_close(sock);
    return rc;
}
