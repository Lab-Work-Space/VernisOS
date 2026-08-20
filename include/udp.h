#ifndef VERNISOS_UDP_H
#define VERNISOS_UDP_H

#include <stdint.h>

// Phase 50: UDP sockets. Phase 51: DHCP + DNS clients built on top.

#define UDP_MAX_SOCKETS   8
#define UDP_RX_QUEUE      4
#define UDP_MAX_PAYLOAD   1472   // 1500 MTU - 20 IP - 8 UDP

// IP-layer transmit callback: sends one UDP datagram (header included).
// src_ip 0 is allowed (DHCP DISCOVER before we have an address);
// dst_ip 0xFFFFFFFF broadcasts (no ARP). IPs in host byte order.
// Must not block; returns 0 on success, <0 if dropped.
typedef int (*UdpIpOutputFn)(uint32_t src_ip, uint32_t dst_ip, const void *seg, int len);

void udp_init(void);
void udp_set_output(UdpIpOutputFn fn, uint32_t local_ip);
void udp_set_local_ip(uint32_t ip);

// port 0 = pick an ephemeral port. Returns socket id or -1.
int  udp_bind(uint16_t port);
int  udp_close(int sock);
uint16_t udp_local_port(int sock);
int  udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void *buf, int len);
// Non-blocking: 0 = no datagram queued, -1 = bad socket, else length.
int  udp_recvfrom(int sock, void *buf, int maxlen, uint32_t *src_ip, uint16_t *src_port);

// Entry point for incoming UDP; IPs in host byte order, seg points at the
// UDP header.
void udp_receive_packet(uint32_t src_ip, uint32_t dst_ip, const void *seg, int len);

// ---- Phase 51: DHCP client ----
// Blocking (CLI context only — the timer IRQ feeds the rx path).
// On success fills out params (host byte order) and returns 0.
int dhcp_run(uint32_t *out_ip, uint32_t *out_mask, uint32_t *out_gw, uint32_t *out_dns);

// ---- Phase 51: DNS client ----
void     dns_set_server(uint32_t ip);
uint32_t dns_get_server(void);
// Blocking A-record lookup; returns 0 and fills out_ip on success.
int dns_resolve(const char *name, uint32_t *out_ip);

#endif // VERNISOS_UDP_H
