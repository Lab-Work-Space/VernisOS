// Phase 49: TCP — state machine, handshake, checksum, minimal data path.
// Arch-agnostic: the kernel registers an IP-layer transmit callback via
// tcp_set_output(); incoming segments arrive through tcp_receive_packet().
#include <stdint.h>
#include "tcp.h"

// Kernel does not have <string.h>, use forward declarations
void *memset(void *s, int c, unsigned long n);
extern uint32_t get_kernel_tick(void);

TcpControlBlock g_tcbs[TCP_MAX_SOCKETS];

static TcpIpOutputFn g_ip_output = 0;
static uint32_t g_local_ip = 0;
static uint16_t g_next_ephemeral = 49152;

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
    // ...payload follows
} __attribute__((packed)) TcpHeader;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

static uint16_t t_htons(uint16_t h) { return (uint16_t)((h >> 8) | (h << 8)); }
static uint32_t t_htonl(uint32_t h) {
    return ((h >> 24) & 0xFF) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | (h << 24);
}
#define t_ntohs t_htons
#define t_ntohl t_htonl

// RFC 793 checksum: pseudo-header (src, dst, proto 6, TCP length) + segment.
// IPs in host byte order; result is in network byte order, store directly.
uint16_t tcp_calc_checksum(const void *packet, int len, uint32_t src_ip, uint32_t dst_ip) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += 6;                 // protocol
    sum += (uint32_t)len;     // TCP length (header + data)
    const uint8_t *p = (const uint8_t *)packet;
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return t_htons((uint16_t)~sum);
}

// Build one segment and hand it to the IP layer. ack field carries
// tcb->ack_num whenever ACK flag is set. The advertised window is computed
// from the current rx buffer fill (Phase 66).
static int tcp_output_seq(TcpControlBlock *tcb, uint8_t flags,
                          const void *payload, int len, uint32_t seq) {
    static uint8_t seg[sizeof(TcpHeader) + TCP_MAX_SEG];
    if (!g_ip_output || len < 0 || len > TCP_MAX_SEG) return -1;
    TcpHeader *h = (TcpHeader *)seg;
    tcb->window = (uint16_t)(TCP_RX_BUF_SIZE - tcb->rx_len);
    h->src_port    = t_htons(tcb->local_port);
    h->dst_port    = t_htons(tcb->remote_port);
    h->seq_num     = t_htonl(seq);
    h->ack_num     = (flags & TCP_FLAG_ACK) ? t_htonl(tcb->ack_num) : 0;
    h->data_offset = 5 << 4;
    h->flags       = flags;
    h->window      = t_htons(tcb->window);
    h->checksum    = 0;
    h->urgent_ptr  = 0;
    const uint8_t *s = (const uint8_t *)payload;
    for (int i = 0; i < len; i++) seg[sizeof(TcpHeader) + i] = s[i];
    h->checksum = tcp_calc_checksum(seg, (int)sizeof(TcpHeader) + len,
                                    g_local_ip, tcb->remote_ip);
    return g_ip_output(tcb->remote_ip, seg, (int)sizeof(TcpHeader) + len);
}

static int tcp_output(TcpControlBlock *tcb, uint8_t flags, const void *payload, int len) {
    return tcp_output_seq(tcb, flags, payload, len, tcb->seq_num);
}

// Phase 66: retransmit the first unacked segment (go-back-N head) from the
// tx ring. Payload must be linearized for tcp_output_seq.
static void tcp_retransmit_head(TcpControlBlock *tcb) {
    static uint8_t lin[TCP_MAX_SEG];
    uint32_t inflight = tcb->seq_num - tcb->snd_una;
    int n = inflight < TCP_MAX_SEG ? (int)inflight : TCP_MAX_SEG;
    for (int i = 0; i < n; i++)
        lin[i] = tcb->tx_buf[(tcb->tx_start + i) % TCP_TX_BUF_SIZE];
    tcp_output_seq(tcb, TCP_FLAG_PSH | TCP_FLAG_ACK, lin, n, tcb->snd_una);
}

// Phase 66: process an incoming ACK — slide the send window.
static void tcp_ack_advance(TcpControlBlock *tcb, uint32_t ack, uint16_t wnd) {
    uint32_t newly = ack - tcb->snd_una;
    uint32_t inflight = tcb->seq_num - tcb->snd_una;
    if (newly > 0 && newly <= inflight) {
        tcb->snd_una = ack;
        tcb->tx_start = (uint16_t)((tcb->tx_start + newly) % TCP_TX_BUF_SIZE);
        tcb->retransmit_count = 0;
        tcb->rto_tick = get_kernel_tick();
    }
    tcb->remote_window = wnd;
}

static uint32_t tcp_pick_isn(int sock) {
    // Timer-based ISN (RFC 793 suggests a clock-driven generator)
    return (get_kernel_tick() << 6) + (uint32_t)sock * 64 + 1;
}

void tcp_send_syn(int sock) {
    TcpControlBlock *tcb = &g_tcbs[sock];
    tcp_output(tcb, TCP_FLAG_SYN, 0, 0);
    tcb->last_activity_tick = get_kernel_tick();
}

void tcp_send_synack(int sock) {
    TcpControlBlock *tcb = &g_tcbs[sock];
    tcp_output(tcb, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
    tcb->last_activity_tick = get_kernel_tick();
}

void tcp_send_ack(int sock) {
    TcpControlBlock *tcb = &g_tcbs[sock];
    tcp_output(tcb, TCP_FLAG_ACK, 0, 0);
}

void tcp_init() {
    memset(g_tcbs, 0, sizeof(g_tcbs));
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        g_tcbs[i].state = TCP_CLOSED;
    }
}

void tcp_set_output(TcpIpOutputFn fn, uint32_t local_ip) {
    g_ip_output = fn;
    g_local_ip = local_ip;
}

int tcp_listen(uint16_t port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        if (g_tcbs[i].state == TCP_CLOSED) {
            memset(&g_tcbs[i], 0, sizeof(g_tcbs[i]));
            g_tcbs[i].state = TCP_LISTEN;
            g_tcbs[i].local_ip = g_local_ip;
            g_tcbs[i].local_port = port;
            g_tcbs[i].window = TCP_RX_BUF_SIZE;
            return i;
        }
    }
    return -1;
}

int tcp_connect(uint32_t ip, uint16_t port) {
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        if (g_tcbs[i].state == TCP_CLOSED) {
            TcpControlBlock *tcb = &g_tcbs[i];
            memset(tcb, 0, sizeof(*tcb));
            tcb->state = TCP_SYN_SENT;
            tcb->local_ip = g_local_ip;
            tcb->local_port = g_next_ephemeral++;
            if (g_next_ephemeral < 49152) g_next_ephemeral = 49152;
            tcb->remote_ip = ip;
            tcb->remote_port = port;
            tcb->seq_num = tcp_pick_isn(i);
            tcb->window = TCP_RX_BUF_SIZE;
            tcp_send_syn(i);
            return i;
        }
    }
    return -1;
}

// Phase 66: sliding-window send. Accepts at most what the peer's window and
// the tx ring have room for; returns bytes queued (0 = try again later).
// Data is buffered in tx_buf until acked, so a segment dropped on the wire
// (or on an ARP cache miss) is recovered by the RTO in tcp_tick().
int tcp_send(int sock, const void *buf, int len) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    TcpControlBlock *tcb = &g_tcbs[sock];
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) return -1;
    if (len <= 0) return 0;
    uint32_t inflight = tcb->seq_num - tcb->snd_una;
    int space = TCP_TX_BUF_SIZE - (int)inflight;
    int allow = (int)tcb->remote_window - (int)inflight;
    if (allow <= 0 || space <= 0) return 0; // window/buffer full: caller retries
    if (len > allow) len = allow;
    if (len > space) len = space;
    if (len > TCP_MAX_SEG) len = TCP_MAX_SEG;
    const uint8_t *s = (const uint8_t *)buf;
    uint16_t w = (uint16_t)((tcb->tx_start + inflight) % TCP_TX_BUF_SIZE);
    for (int i = 0; i < len; i++)
        tcb->tx_buf[(w + i) % TCP_TX_BUF_SIZE] = s[i];
    // Transmit failure is fine: the bytes are queued and the RTO resends.
    tcp_output(tcb, TCP_FLAG_PSH | TCP_FLAG_ACK, buf, len);
    tcb->seq_num += (uint32_t)len;
    tcb->rto_tick = get_kernel_tick();
    return len;
}

int tcp_recv(int sock, void *buf, int maxlen) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || maxlen < 0) return -1;
    TcpControlBlock *tcb = &g_tcbs[sock];
    if (tcb->rx_len == 0) {
        // No data: 0 while the peer can still send, -1 once it can't
        return (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
                tcb->state == TCP_FIN_WAIT_2) ? 0 : -1;
    }
    int n = tcb->rx_len < maxlen ? tcb->rx_len : maxlen;
    uint8_t *d = (uint8_t *)buf;
    int was_full = (TCP_RX_BUF_SIZE - tcb->rx_len) < TCP_MAX_SEG;
    for (int i = 0; i < n; i++) d[i] = tcb->rx_buf[i];
    for (int i = n; i < tcb->rx_len; i++) tcb->rx_buf[i - n] = tcb->rx_buf[i];
    tcb->rx_len -= (uint16_t)n;
    // Phase 66: the advertised window was (nearly) closed; tell the peer it
    // reopened so it doesn't sit in RTO backoff.
    if (was_full && n > 0 &&
        (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
         tcb->state == TCP_FIN_WAIT_2))
        tcp_send_ack(sock);
    return n;
}

int tcp_close(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    TcpControlBlock *tcb = &g_tcbs[sock];
    switch (tcb->state) {
    case TCP_ESTABLISHED:
    case TCP_SYN_RECEIVED:
        tcp_output(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        tcb->state = TCP_FIN_WAIT_1;
        tcb->last_activity_tick = get_kernel_tick();
        break;
    case TCP_CLOSE_WAIT:
        tcp_output(tcb, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        tcb->state = TCP_LAST_ACK;
        tcb->last_activity_tick = get_kernel_tick();
        break;
    default:
        tcb->state = TCP_CLOSED;
        break;
    }
    return 0;
}

void tcp_tick() {
    uint32_t now = get_kernel_tick();
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        TcpControlBlock *tcb = &g_tcbs[i];
        if (tcb->state == TCP_SYN_SENT || tcb->state == TCP_SYN_RECEIVED) {
            // Handshake retransmission (also re-sends segments dropped on
            // ARP cache miss in the IP output path)
            if (now - tcb->last_activity_tick > 120) { // ~0.5s at 240Hz
                if (tcb->retransmit_count < 5) {
                    if (tcb->state == TCP_SYN_SENT) tcp_send_syn(i);
                    else tcp_send_synack(i);
                    tcb->retransmit_count++;
                    tcb->last_activity_tick = now;
                } else {
                    tcb->state = TCP_CLOSED;
                }
            }
        } else if (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) {
            // Phase 66: go-back-N data retransmission with exponential backoff
            uint32_t inflight = tcb->seq_num - tcb->snd_una;
            if (inflight > 0) {
                uint8_t shift = tcb->retransmit_count < 4 ? tcb->retransmit_count : 4;
                if (now - tcb->rto_tick > (120u << shift)) { // base RTO ~0.5s
                    if (tcb->retransmit_count < 8) {
                        tcp_retransmit_head(tcb);
                        tcb->retransmit_count++;
                        tcb->rto_tick = now;
                    } else {
                        tcb->state = TCP_CLOSED; // peer unreachable: give up
                    }
                }
            }
        } else if (tcb->state == TCP_FIN_WAIT_1 || tcb->state == TCP_FIN_WAIT_2 ||
                   tcb->state == TCP_LAST_ACK || tcb->state == TCP_CLOSING ||
                   tcb->state == TCP_TIME_WAIT) {
            // Teardown states: give up after ~5s instead of lingering forever
            if (now - tcb->last_activity_tick > 1200) {
                tcb->state = TCP_CLOSED;
            }
        }
    }
}

const char *tcp_state_name(TcpState s) {
    switch (s) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_CLOSING:      return "CLOSING";
    case TCP_LAST_ACK:     return "LAST_ACK";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    default:               return "?";
    }
}

static void tcp_handle_segment(int i, uint32_t src_ip, const TcpHeader *hdr,
                               const uint8_t *data, int dlen) {
    TcpControlBlock *tcb = &g_tcbs[i];
    uint32_t seq = t_ntohl(hdr->seq_num);
    uint32_t ack = t_ntohl(hdr->ack_num);
    uint8_t flags = hdr->flags;

    if (flags & TCP_FLAG_RST) {
        tcb->state = TCP_CLOSED;
        return;
    }

    switch (tcb->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)
            && ack == tcb->seq_num + 1) {
            tcb->seq_num++;
            tcb->ack_num = seq + 1;
            tcb->remote_window = t_ntohs(hdr->window);
            tcb->snd_una = tcb->seq_num; // Phase 66: send window starts empty
            tcb->tx_start = 0;
            tcb->rto_tick = get_kernel_tick();
            tcb->state = TCP_ESTABLISHED;
            tcp_send_ack(i);
        }
        break;

    case TCP_SYN_RECEIVED:
        if ((flags & TCP_FLAG_ACK) && ack == tcb->seq_num + 1) {
            tcb->seq_num++;
            tcb->remote_window = t_ntohs(hdr->window);
            tcb->snd_una = tcb->seq_num; // Phase 66
            tcb->tx_start = 0;
            tcb->rto_tick = get_kernel_tick();
            tcb->state = TCP_ESTABLISHED;
        }
        break;

    case TCP_ESTABLISHED: {
        int acked = 0;
        if (flags & TCP_FLAG_ACK) // Phase 66: slide the send window
            tcp_ack_advance(tcb, ack, t_ntohs(hdr->window));
        if (dlen > 0) {
            if (seq == tcb->ack_num) {
                int space = TCP_RX_BUF_SIZE - tcb->rx_len;
                int store = dlen < space ? dlen : space;
                for (int k = 0; k < store; k++)
                    tcb->rx_buf[tcb->rx_len + k] = data[k];
                tcb->rx_len += (uint16_t)store;
                tcb->ack_num += (uint32_t)store; // unstored tail gets resent

                // The gap just closed — flush a buffered out-of-order
                // segment if it's now contiguous with the new ack_num.
                if (tcb->ooo_valid && tcb->ooo_seq == tcb->ack_num) {
                    space = TCP_RX_BUF_SIZE - tcb->rx_len;
                    store = tcb->ooo_len < space ? tcb->ooo_len : space;
                    for (int k = 0; k < store; k++)
                        tcb->rx_buf[tcb->rx_len + k] = tcb->ooo_buf[k];
                    tcb->rx_len += (uint16_t)store;
                    tcb->ack_num += (uint32_t)store;
                    tcb->ooo_valid = 0;
                }
            } else if (seq > tcb->ack_num && !tcb->ooo_valid) {
                // Ahead of the expected byte: buffer it so the receiver can
                // reassemble once the gap-filler arrives, instead of relying
                // on the sender to retransmit this segment too.
                int store = dlen < TCP_MAX_SEG ? dlen : TCP_MAX_SEG;
                for (int k = 0; k < store; k++)
                    tcb->ooo_buf[k] = data[k];
                tcb->ooo_seq = seq;
                tcb->ooo_len = (uint16_t)store;
                tcb->ooo_valid = 1;
            }
            tcp_send_ack(i); // ACK in-order data; dup-ACK out-of-order
            acked = 1;
        }
        if (flags & TCP_FLAG_FIN) {
            if (seq + (uint32_t)dlen == tcb->ack_num) {
                tcb->ack_num++;
                tcp_send_ack(i);
                acked = 1;
                tcb->state = TCP_CLOSE_WAIT;
            }
        }
        (void)acked;
        break;
    }

    case TCP_CLOSE_WAIT:
        // Phase 66: peer sent FIN but we may still be sending — take ACKs.
        if (flags & TCP_FLAG_ACK)
            tcp_ack_advance(tcb, ack, t_ntohs(hdr->window));
        break;

    case TCP_FIN_WAIT_1:
        if ((flags & TCP_FLAG_ACK) && ack == tcb->seq_num + 1) {
            tcb->seq_num++;
            tcb->state = TCP_FIN_WAIT_2;
        }
        if (flags & TCP_FLAG_FIN) {
            tcb->ack_num = seq + 1;
            tcp_send_ack(i);
            // Simultaneous close or FIN+ACK combined; skip TIME_WAIT
            tcb->state = (tcb->state == TCP_FIN_WAIT_2) ? TCP_CLOSED : TCP_CLOSING;
            tcb->last_activity_tick = get_kernel_tick();
        }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            tcb->ack_num = seq + 1;
            tcp_send_ack(i);
            tcb->state = TCP_CLOSED;
        }
        break;

    case TCP_CLOSING:
        if (flags & TCP_FLAG_ACK) tcb->state = TCP_CLOSED;
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_FLAG_ACK) tcb->state = TCP_CLOSED;
        break;

    default:
        break;
    }
    (void)src_ip;
}

void tcp_receive_packet(uint32_t src_ip, uint32_t dst_ip, const void *packet, int len) {
    (void)dst_ip;
    if (len < (int)sizeof(TcpHeader)) return;
    const TcpHeader *hdr = (const TcpHeader *)packet;
    uint16_t sport = t_ntohs(hdr->src_port);
    uint16_t dport = t_ntohs(hdr->dst_port);
    int hlen = (hdr->data_offset >> 4) * 4;
    if (hlen < (int)sizeof(TcpHeader) || hlen > len) return;
    const uint8_t *data = (const uint8_t *)packet + hlen;
    int dlen = len - hlen;

    // Pass 1: exact 4-tuple match on a non-listening socket
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        TcpControlBlock *tcb = &g_tcbs[i];
        if (tcb->state != TCP_CLOSED && tcb->state != TCP_LISTEN &&
            tcb->local_port == dport && tcb->remote_port == sport &&
            tcb->remote_ip == src_ip) {
            tcp_handle_segment(i, src_ip, hdr, data, dlen);
            return;
        }
    }
    // Pass 2: listening socket (passive open). Phase 65: the listener stays
    // in LISTEN and spawns a fresh TCB for the connection, so tcp_accept()
    // can hand out established connections while the listener keeps serving.
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        TcpControlBlock *tcb = &g_tcbs[i];
        if (tcb->state == TCP_LISTEN && tcb->local_port == dport &&
            (hdr->flags & TCP_FLAG_SYN) && !(hdr->flags & TCP_FLAG_ACK)) {
            int c = -1;
            for (int k = 0; k < TCP_MAX_SOCKETS; ++k) {
                if (g_tcbs[k].state == TCP_CLOSED) { c = k; break; }
            }
            if (c < 0) return; // no free TCB: drop, peer retransmits the SYN
            TcpControlBlock *child = &g_tcbs[c];
            memset(child, 0, sizeof(*child));
            child->state = TCP_SYN_RECEIVED;
            child->local_ip = tcb->local_ip;
            child->local_port = dport;
            child->remote_ip = src_ip;
            child->remote_port = sport;
            child->seq_num = tcp_pick_isn(c);
            child->ack_num = t_ntohl(hdr->seq_num) + 1;
            child->remote_window = t_ntohs(hdr->window);
            child->window = TCP_RX_BUF_SIZE;
            child->from_listener = 1;
            tcp_send_synack(c);
            return;
        }
    }
}

int tcp_accept(int listen_sock) {
    if (listen_sock < 0 || listen_sock >= TCP_MAX_SOCKETS) return -1;
    TcpControlBlock *l = &g_tcbs[listen_sock];
    if (l->state != TCP_LISTEN) return -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; ++i) {
        TcpControlBlock *tcb = &g_tcbs[i];
        if (tcb->from_listener && !tcb->accepted &&
            tcb->local_port == l->local_port &&
            (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT)) {
            tcb->accepted = 1;
            return i;
        }
    }
    return -1;
}
