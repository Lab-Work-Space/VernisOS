// Host-side deterministic test for TCP out-of-order reassembly.
// Compiles kernel/net/tcp.c directly on macOS with mocked get_kernel_tick +
// a mocked IP output function (see tester.md) — no QEMU needed.
//
//   cc -std=c11 -Wall -Iinclude -o /tmp/test_tcp_ooo \
//       kernel/net/tcp.c kernel/net/tests/test_tcp_ooo.c && /tmp/test_tcp_ooo
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tcp.h"

static uint32_t g_tick = 1000;
uint32_t get_kernel_tick(void) { return g_tick; }

static int mock_output(uint32_t dst_ip, const void *seg, int len) {
    (void)dst_ip; (void)seg; (void)len;
    return 0;
}

// Mirrors the private TcpHeader layout in kernel/net/tcp.c so the test can
// hand-build wire segments without exposing that struct in tcp.h.
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) TestTcpHeader;

#define T_FLAG_SYN 0x02
#define T_FLAG_ACK 0x10
#define T_FLAG_PSH 0x08

static uint16_t swap16(uint16_t h) { return (uint16_t)((h >> 8) | (h << 8)); }
static uint32_t swap32(uint32_t h) {
    return ((h >> 24) & 0xFF) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | (h << 24);
}

static int build_segment(uint8_t *out, uint16_t sport, uint16_t dport,
                          uint32_t seq, uint32_t ack, uint8_t flags,
                          const void *payload, int plen) {
    TestTcpHeader *h = (TestTcpHeader *)out;
    h->src_port    = swap16(sport);
    h->dst_port    = swap16(dport);
    h->seq_num     = swap32(seq);
    h->ack_num     = swap32(ack);
    h->data_offset = 5 << 4;
    h->flags       = flags;
    h->window      = swap16(4096);
    h->checksum    = 0;
    h->urgent_ptr  = 0;
    if (plen > 0) memcpy(out + sizeof(TestTcpHeader), payload, plen);
    return (int)sizeof(TestTcpHeader) + plen;
}

#define CLIENT_IP   0x0A000002u
#define LOCAL_IP    0x0A00000Fu
#define CLIENT_PORT 55000
#define SERVER_PORT 7777

static TcpControlBlock *find_child(uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (g_tcbs[i].from_listener && g_tcbs[i].remote_port == remote_port)
            return &g_tcbs[i];
    }
    return NULL;
}

int main(void) {
    uint8_t pkt[128];
    int len;

    tcp_init();
    tcp_set_output(mock_output, LOCAL_IP);

    int lsock = tcp_listen(SERVER_PORT);
    assert(lsock >= 0);

    uint32_t client_isn = 100;
    len = build_segment(pkt, CLIENT_PORT, SERVER_PORT, client_isn, 0, T_FLAG_SYN, 0, 0);
    tcp_receive_packet(CLIENT_IP, LOCAL_IP, pkt, len);

    TcpControlBlock *tcb = find_child(CLIENT_PORT);
    assert(tcb != NULL);
    assert(tcb->state == TCP_SYN_RECEIVED);
    uint32_t server_isn = tcb->seq_num;

    len = build_segment(pkt, CLIENT_PORT, SERVER_PORT, client_isn + 1,
                         server_isn + 1, T_FLAG_ACK, 0, 0);
    tcp_receive_packet(CLIENT_IP, LOCAL_IP, pkt, len);
    assert(tcb->state == TCP_ESTABLISHED);

    uint32_t base = tcb->ack_num;
    assert(base == client_isn + 1);

    // Out-of-order: "WORLD" (seq base+5) arrives BEFORE "HELLO" (seq base).
    len = build_segment(pkt, CLIENT_PORT, SERVER_PORT, base + 5, server_isn + 1,
                         T_FLAG_PSH | T_FLAG_ACK, "WORLD", 5);
    tcp_receive_packet(CLIENT_IP, LOCAL_IP, pkt, len);

    // Gap-filler arrives; receiver must reassemble both without requiring
    // the sender to retransmit "WORLD".
    len = build_segment(pkt, CLIENT_PORT, SERVER_PORT, base, server_isn + 1,
                         T_FLAG_PSH | T_FLAG_ACK, "HELLO", 5);
    tcp_receive_packet(CLIENT_IP, LOCAL_IP, pkt, len);

    int sock = (int)(tcb - g_tcbs);
    uint8_t out[64] = {0};
    int n = tcp_recv(sock, out, sizeof(out));
    printf("recv: %d bytes: '%.*s'\n", n, n, out);
    assert(n == 10);
    assert(memcmp(out, "HELLOWORLD", 10) == 0);
    assert(tcb->ack_num == base + 10);

    printf("PASS: TCP out-of-order reassembly\n");
    return 0;
}
