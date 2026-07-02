#ifndef VERNISOS_TCP_H
#define VERNISOS_TCP_H

#include <stdint.h>

#define TCP_MAX_SOCKETS 16
#define TCP_RX_BUF_SIZE 1024
#define TCP_MAX_SEG     1024

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} TcpState;


typedef struct TcpControlBlock {
    TcpState state;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t window;
    uint16_t remote_window;
    uint8_t  retransmit_count;
    uint32_t last_activity_tick;
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint16_t rx_len;
} TcpControlBlock;

extern TcpControlBlock g_tcbs[TCP_MAX_SOCKETS];

// IP-layer transmit callback: sends one TCP segment to dst_ip (host byte
// order). Must not block; returns 0 on success, <0 if the segment was
// dropped (e.g. ARP cache miss — retransmission covers it).
typedef int (*TcpIpOutputFn)(uint32_t dst_ip, const void *segment, int len);

void tcp_init();
void tcp_set_output(TcpIpOutputFn fn, uint32_t local_ip);
int  tcp_listen(uint16_t port);
int  tcp_connect(uint32_t ip, uint16_t port);
int  tcp_send(int sock, const void *buf, int len);
int  tcp_recv(int sock, void *buf, int maxlen);
int  tcp_close(int sock);
void tcp_tick();
const char *tcp_state_name(TcpState s);

// Entry point for incoming TCP segments; IPs in host byte order,
// packet points at the TCP header.
void tcp_receive_packet(uint32_t src_ip, uint32_t dst_ip, const void *packet, int len);

#endif // VERNISOS_TCP_H
