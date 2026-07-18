#include <kernel/socket.h>
#include <kernel/netif.h>
#include <kernel/netstack.h>
#include <kernel/errno.h>
#include <kernel/string.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <arch/x86/irq.h>

#define SOCK_MAX             16
#define SOCK_RX_SLOTS        8
#define SOCK_RX_BYTES        1472
#define SOCK_ACCEPT_SLOTS    4
#define SOCK_STREAM_RX_BYTES 4096

#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECV     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT1    5
#define TCP_FIN_WAIT2    6
#define TCP_CLOSE_WAIT   7
#define TCP_LAST_ACK     8

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

typedef struct {
    uint32_t src_ip;   /* host order */
    uint16_t src_port; /* host order */
    uint16_t len;
    uint8_t  data[SOCK_RX_BYTES];
} sock_pkt_t;

typedef struct {
    int      used;
    int      domain;
    int      type;
    int      proto;
    int      bound;
    int      connected;
    int      state;
    int      backlog;
    int      parent_sid;
    uint8_t  shut_rd;
    uint8_t  shut_wr;
    int      error;
    uint16_t lport; /* host */
    uint32_t laddr; /* host */
    uint16_t rport; /* host */
    uint32_t raddr; /* host */
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t irs;
    uint32_t rcv_nxt;
    sock_pkt_t rx[SOCK_RX_SLOTS];
    int      rx_r;
    int      rx_w;
    int      rx_n;
    uint8_t  stream_rx[SOCK_STREAM_RX_BYTES];
    size_t   stream_rx_len;
    int      accept_q[SOCK_ACCEPT_SLOTS];
    int      aq_r;
    int      aq_w;
    int      aq_n;
    pid_t    waiter; /* blocked thread waiting for RX/accept/connect */
} socket_t;

static socket_t g_socks[SOCK_MAX];
static uint16_t g_ephemeral = 40000;
static uint32_t g_tcp_iss = 0x1000u;

static void sock_reset(socket_t *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->parent_sid = -1;
    s->waiter = -1;
}

static void sock_wake_waiter(socket_t *s)
{
    process_t *p;
    if (!s || s->waiter <= 0)
        return;
    p = process_by_tid(s->waiter);
    s->waiter = -1;
    if (p)
        process_wake(p);
}

/* Deschedule until woken; short tick backup so connect/ack cannot hang forever. */
static void sock_block_wait(socket_t *s)
{
    process_t *cur = process_current();
    if (!s || !cur)
        return;
    s->waiter = cur->tid > 0 ? cur->tid : cur->pid;
    process_block(irq_timer_ticks() + 2);
    schedule();
}

void socket_init(void)
{
    int i;
    memset(g_socks, 0, sizeof(g_socks));
    for (i = 0; i < SOCK_MAX; i++)
        g_socks[i].parent_sid = -1;
    g_ephemeral = 40000;
    g_tcp_iss = 0x1000u;
}

static socket_t *sock_get(int sid)
{
    if (sid < 0 || sid >= SOCK_MAX || !g_socks[sid].used)
        return NULL;
    return &g_socks[sid];
}

static int sock_alloc(int domain, int type, int proto)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        if (!g_socks[i].used) {
            sock_reset(&g_socks[i]);
            g_socks[i].used = 1;
            g_socks[i].domain = domain;
            g_socks[i].type = type;
            g_socks[i].proto = proto;
            return i;
        }
    }
    return -EMFILE;
}

static int port_in_use(int type, int proto, uint16_t port, uint32_t laddr)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        socket_t *s = &g_socks[i];
        if (!s->used || !s->bound)
            continue;
        if (s->type != type || s->proto != proto)
            continue;
        if (s->lport == port && (s->laddr == 0 || laddr == 0 || s->laddr == laddr))
            return 1;
    }
    return 0;
}

static uint16_t alloc_ephemeral(int type, int proto)
{
    int n;
    for (n = 0; n < 20000; n++) {
        uint16_t p = g_ephemeral++;
        if (g_ephemeral < 40000)
            g_ephemeral = 40000;
        if (!port_in_use(type, proto, p, 0))
            return p;
    }
    return 0;
}

static int rx_push(socket_t *s, uint32_t src_ip, uint16_t src_port,
                   const void *data, size_t len)
{
    sock_pkt_t *p;
    if (s->rx_n >= SOCK_RX_SLOTS)
        return -1;
    if (len > SOCK_RX_BYTES)
        len = SOCK_RX_BYTES;
    p = &s->rx[s->rx_w];
    p->src_ip = src_ip;
    p->src_port = src_port;
    p->len = (uint16_t)len;
    memcpy(p->data, data, len);
    s->rx_w = (s->rx_w + 1) % SOCK_RX_SLOTS;
    s->rx_n++;
    sock_wake_waiter(s);
    return 0;
}

static int rx_pop(socket_t *s, sock_pkt_t *out)
{
    if (s->rx_n == 0)
        return -1;
    *out = s->rx[s->rx_r];
    s->rx_r = (s->rx_r + 1) % SOCK_RX_SLOTS;
    s->rx_n--;
    return 0;
}

static int stream_push(socket_t *s, const void *data, size_t len)
{
    if (!s || !data)
        return -EINVAL;
    if (len > sizeof(s->stream_rx) - s->stream_rx_len)
        return -EAGAIN;
    memcpy(s->stream_rx + s->stream_rx_len, data, len);
    s->stream_rx_len += len;
    sock_wake_waiter(s);
    return 0;
}

static size_t stream_pop(socket_t *s, void *buf, size_t len)
{
    if (!s || !buf || s->stream_rx_len == 0)
        return 0;
    if (len > s->stream_rx_len)
        len = s->stream_rx_len;
    memcpy(buf, s->stream_rx, len);
    if (len < s->stream_rx_len)
        memmove(s->stream_rx, s->stream_rx + len, s->stream_rx_len - len);
    s->stream_rx_len -= len;
    return len;
}

static int acceptq_push(socket_t *listener, int child_sid)
{
    if (!listener || listener->aq_n >= SOCK_ACCEPT_SLOTS)
        return -1;
    listener->accept_q[listener->aq_w] = child_sid;
    listener->aq_w = (listener->aq_w + 1) % SOCK_ACCEPT_SLOTS;
    listener->aq_n++;
    sock_wake_waiter(listener);
    return 0;
}

static int acceptq_pop(socket_t *listener)
{
    int sid;
    if (!listener || listener->aq_n == 0)
        return -1;
    sid = listener->accept_q[listener->aq_r];
    listener->aq_r = (listener->aq_r + 1) % SOCK_ACCEPT_SLOTS;
    listener->aq_n--;
    return sid;
}

static int ensure_bound(socket_t *s, int sid)
{
    sockaddr_in_t any;
    if (s->bound)
        return 0;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    return sock_bind(sid, &any);
}

static uint32_t tcp_next_iss(void)
{
    g_tcp_iss += 0x1000u;
    return g_tcp_iss;
}

static uint32_t tcp_local_ip(socket_t *s, netif_t *nif)
{
    return (s && s->laddr != 0) ? s->laddr : (nif ? nif->ip : 0);
}

static int tcp_send_segment(socket_t *s, uint32_t seq, uint32_t ack,
                            uint8_t flags, const void *payload, size_t len)
{
    netif_t *nif = netif_default();
    uint32_t lip;
    if (!s || !nif || !nif->up)
        return -ENETDOWN;
    lip = tcp_local_ip(s, nif);
    if (lip == 0 || s->raddr == 0 || s->lport == 0 || s->rport == 0)
        return -EINVAL;
    return tcp_output(nif, lip, s->lport, s->raddr, s->rport, seq, ack, flags, 4096u, payload, len);
}

static int tcp_ack_now(socket_t *s)
{
    return tcp_send_segment(s, s->snd_nxt, s->rcv_nxt, TCP_FLAG_ACK, NULL, 0);
}

static int tcp_send_control(socket_t *s, uint8_t flags)
{
    int rc = tcp_send_segment(s, s->snd_nxt, s->rcv_nxt, flags, NULL, 0);
    if (rc < 0)
        return rc;
    if (flags & TCP_FLAG_SYN)
        s->snd_nxt++;
    if (flags & TCP_FLAG_FIN)
        s->snd_nxt++;
    return 0;
}

static socket_t *tcp_find_conn(uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        socket_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_STREAM || !s->bound)
            continue;
        if (s->state == TCP_LISTEN)
            continue;
        if (s->lport != dst_port || s->rport != src_port)
            continue;
        if ((s->laddr != 0 && s->laddr != dst_ip) || s->raddr != src_ip)
            continue;
        return s;
    }
    return NULL;
}

static int tcp_find_sid(const socket_t *target)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        if (&g_socks[i] == target)
            return i;
    }
    return -1;
}

static socket_t *tcp_find_listener(uint32_t dst_ip, uint16_t dst_port)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        socket_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_STREAM || s->state != TCP_LISTEN)
            continue;
        if (s->lport != dst_port)
            continue;
        if (s->laddr != 0 && s->laddr != dst_ip)
            continue;
        return s;
    }
    return NULL;
}

static void tcp_mark_error(socket_t *s, int err)
{
    if (!s)
        return;
    s->error = err;
    if (s->state == TCP_SYN_SENT)
        s->connected = 0;
    s->state = TCP_CLOSED;
    sock_wake_waiter(s);
}

static void tcp_try_queue_child(socket_t *child)
{
    socket_t *parent;
    if (!child || child->parent_sid < 0)
        return;
    parent = sock_get(child->parent_sid);
    if (!parent || parent->state != TCP_LISTEN)
        return;
    if (acceptq_push(parent, tcp_find_sid(child)) == 0)
        child->parent_sid = -1;
}

static void tcp_update_ack_state(socket_t *s, uint32_t ack)
{
    if (!s)
        return;
    if (ack > s->snd_una && ack <= s->snd_nxt)
        s->snd_una = ack;
    if (s->state == TCP_SYN_RECV && s->snd_una == s->snd_nxt) {
        s->state = TCP_ESTABLISHED;
        tcp_try_queue_child(s);
    } else if (s->state == TCP_FIN_WAIT1 && s->snd_una == s->snd_nxt) {
        s->state = TCP_FIN_WAIT2;
    } else if (s->state == TCP_LAST_ACK && s->snd_una == s->snd_nxt) {
        tcp_mark_error(s, 0);
    }
    sock_wake_waiter(s);
}

static int tcp_wait_connected(socket_t *s)
{
    int tries;
    for (tries = 0; tries < 500; tries++) {
        if (s->state == TCP_ESTABLISHED)
            return 0;
        if (s->error)
            return s->error;
        if (s->state == TCP_CLOSED)
            return -ETIMEDOUT;
        net_poll();
        sock_block_wait(s);
    }
    return -ETIMEDOUT;
}

static int tcp_wait_ack(socket_t *s, uint32_t want_ack)
{
    int tries;
    for (tries = 0; tries < 500; tries++) {
        if (s->snd_una >= want_ack)
            return 0;
        if (s->error)
            return s->error;
        if (s->state == TCP_CLOSED)
            return -EPIPE;
        net_poll();
        sock_block_wait(s);
    }
    return -ETIMEDOUT;
}

static int tcp_wait_accept(socket_t *s)
{
    for (;;) {
        if (s->aq_n > 0)
            return 0;
        net_poll();
        sock_block_wait(s);
    }
}

int sock_create(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type == SOCK_DGRAM) {
        if (protocol == 0)
            protocol = IPPROTO_UDP;
        if (protocol != IPPROTO_UDP)
            return -EPROTONOSUPPORT;
    } else if (type == SOCK_RAW) {
        if (protocol == 0)
            protocol = IPPROTO_ICMP;
        if (protocol != IPPROTO_ICMP)
            return -EPROTONOSUPPORT;
    } else if (type == SOCK_STREAM) {
        if (protocol == 0)
            protocol = IPPROTO_TCP;
        if (protocol != IPPROTO_TCP)
            return -EPROTONOSUPPORT;
    } else {
        return -EPROTONOSUPPORT;
    }
    return sock_alloc(domain, type, protocol);
}

int sock_bind(int sid, const sockaddr_in_t *addr)
{
    socket_t *s = sock_get(sid);
    uint16_t port;
    uint32_t lip;
    if (!s || !addr)
        return -EINVAL;
    if (addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    port = ntohs(addr->sin_port);
    lip = ntohl(addr->sin_addr);
    if (port == 0) {
        port = alloc_ephemeral(s->type, s->proto);
        if (!port)
            return -EADDRINUSE;
    } else if (port_in_use(s->type, s->proto, port, lip)) {
        return -EADDRINUSE;
    }
    s->lport = port;
    s->laddr = lip;
    s->bound = 1;
    return 0;
}

int sock_connect(int sid, const sockaddr_in_t *addr)
{
    socket_t *s = sock_get(sid);
    int rc;
    if (!s || !addr)
        return -EINVAL;
    if (addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    if (!s->bound) {
        sockaddr_in_t any;
        memset(&any, 0, sizeof(any));
        any.sin_family = AF_INET;
        any.sin_port = 0;
        any.sin_addr = htonl(INADDR_ANY);
        if (sock_bind(sid, &any) < 0)
            return -EADDRINUSE;
    }
    s->raddr = ntohl(addr->sin_addr);
    s->rport = ntohs(addr->sin_port);
    s->connected = 1;
    s->error = 0;

    if (s->type != SOCK_STREAM)
        return 0;

    s->state = TCP_SYN_SENT;
    s->iss = tcp_next_iss();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss;
    s->irs = 0;
    s->rcv_nxt = 0;
    rc = tcp_send_control(s, TCP_FLAG_SYN);
    if (rc < 0) {
        tcp_mark_error(s, rc);
        return rc;
    }
    rc = tcp_wait_connected(s);
    if (rc < 0) {
        tcp_mark_error(s, rc);
        return rc;
    }
    return 0;
}

int sock_listen(int sid, int backlog)
{
    socket_t *s = sock_get(sid);
    if (!s || s->type != SOCK_STREAM)
        return -EINVAL;
    if (!s->bound)
        return -EINVAL;
    if (backlog < 0)
        backlog = 0;
    if (backlog > SOCK_ACCEPT_SLOTS)
        backlog = SOCK_ACCEPT_SLOTS;
    s->backlog = backlog;
    s->state = TCP_LISTEN;
    return 0;
}

int sock_accept(int sid, sockaddr_in_t *addr)
{
    socket_t *s = sock_get(sid);
    socket_t *child;
    int child_sid;
    if (!s || s->type != SOCK_STREAM || s->state != TCP_LISTEN)
        return -EINVAL;
    if (s->aq_n == 0) {
        int rc = tcp_wait_accept(s);
        if (rc < 0)
            return rc;
    }
    child_sid = acceptq_pop(s);
    if (child_sid < 0)
        return -EAGAIN;
    child = sock_get(child_sid);
    if (!child || child->state != TCP_ESTABLISHED)
        return -EAGAIN;
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons(child->rport);
        addr->sin_addr = htonl(child->raddr);
    }
    return child_sid;
}

ssize_t sock_send(int sid, const void *buf, size_t len, int flags)
{
    socket_t *s = sock_get(sid);
    netif_t *nif = netif_default();
    size_t total = 0;
    size_t mss;
    (void)flags;

    if (!s || !buf)
        return -EINVAL;
    if (s->type != SOCK_STREAM)
        return -EINVAL;
    if (!nif || !nif->up)
        return -ENETDOWN;
    if (!s->connected || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT))
        return -EPIPE;
    if (s->shut_wr)
        return -EPIPE;

    mss = (nif->mtu > 40) ? (size_t)(nif->mtu - 40) : 536u;
    if (mss > 1024u)
        mss = 1024u;

    while (total < len) {
        size_t chunk = len - total;
        uint32_t want_ack;
        int rc;
        if (chunk > mss)
            chunk = mss;
        rc = tcp_send_segment(s, s->snd_nxt, s->rcv_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH,
                              (const uint8_t *)buf + total, chunk);
        if (rc < 0)
            return total > 0 ? (ssize_t)total : rc;
        s->snd_nxt += (uint32_t)chunk;
        want_ack = s->snd_nxt;
        rc = tcp_wait_ack(s, want_ack);
        if (rc < 0)
            return total > 0 ? (ssize_t)total : rc;
        total += chunk;
    }
    return (ssize_t)total;
}

ssize_t sock_recv(int sid, void *buf, size_t len, int flags)
{
    socket_t *s = sock_get(sid);
    int wait = !(flags & MSG_DONTWAIT);
    size_t got;

    if (!s || !buf)
        return -EINVAL;
    if (s->type != SOCK_STREAM)
        return -EINVAL;

    while (s->stream_rx_len == 0) {
        if (s->state == TCP_CLOSE_WAIT || s->state == TCP_CLOSED)
            return 0;
        if (!wait)
            return -EAGAIN;
        if (s->error)
            return s->error;
        net_poll();
        sock_block_wait(s);
    }

    got = stream_pop(s, buf, len);
    return (ssize_t)got;
}

ssize_t sock_sendto(int sid, const void *buf, size_t len, int flags,
                    const sockaddr_in_t *dst)
{
    socket_t *s = sock_get(sid);
    netif_t *nif = netif_default();
    uint32_t dip;
    uint16_t dport;
    (void)flags;

    if (!s || !buf)
        return -EINVAL;
    if (!nif || !nif->up)
        return -ENETDOWN;
    if (ensure_bound(s, sid) < 0)
        return -EADDRINUSE;

    if (s->type == SOCK_STREAM) {
        if (dst) {
            if (dst->sin_family != AF_INET)
                return -EAFNOSUPPORT;
            if (s->raddr != ntohl(dst->sin_addr) || s->rport != ntohs(dst->sin_port))
                return -EDESTADDRREQ;
        }
        return sock_send(sid, buf, len, flags);
    }

    if (dst) {
        if (dst->sin_family != AF_INET)
            return -EAFNOSUPPORT;
        dip = ntohl(dst->sin_addr);
        dport = ntohs(dst->sin_port);
    } else if (s->connected) {
        dip = s->raddr;
        dport = s->rport;
    } else {
        return -EDESTADDRREQ;
    }

    if (s->type == SOCK_DGRAM) {
        if (len > SOCK_RX_BYTES)
            return -EMSGSIZE;
        if (udp_output(nif, tcp_local_ip(s, nif), s->lport, dip, dport, buf, len) < 0)
            return -EHOSTUNREACH;
        return (ssize_t)len;
    }

    if (s->type == SOCK_RAW) {
        if (len > SOCK_RX_BYTES)
            return -EMSGSIZE;
        if (ipv4_output(nif, dip, (uint8_t)s->proto, buf, len) < 0)
            return -EHOSTUNREACH;
        return (ssize_t)len;
    }
    return -EPROTONOSUPPORT;
}

ssize_t sock_recvfrom(int sid, void *buf, size_t len, int flags,
                      sockaddr_in_t *src)
{
    socket_t *s = sock_get(sid);
    sock_pkt_t pkt;
    int wait = !(flags & MSG_DONTWAIT);

    if (!s || !buf)
        return -EINVAL;
    if (ensure_bound(s, sid) < 0)
        return -EADDRINUSE;

    if (s->type == SOCK_STREAM) {
        ssize_t n = sock_recv(sid, buf, len, flags);
        if (n > 0 && src) {
            memset(src, 0, sizeof(*src));
            src->sin_family = AF_INET;
            src->sin_port = htons(s->rport);
            src->sin_addr = htonl(s->raddr);
        }
        return n;
    }

    while (rx_pop(s, &pkt) < 0) {
        if (!wait)
            return -EAGAIN;
        net_poll();
        sock_block_wait(s);
    }

    if (len > pkt.len)
        len = pkt.len;
    memcpy(buf, pkt.data, len);
    if (src) {
        memset(src, 0, sizeof(*src));
        src->sin_family = AF_INET;
        src->sin_port = htons(pkt.src_port);
        src->sin_addr = htonl(pkt.src_ip);
    }
    return (ssize_t)len;
}

int sock_shutdown(int sid, int how)
{
    socket_t *s = sock_get(sid);
    int rc = 0;
    if (!s || s->type != SOCK_STREAM)
        return -EINVAL;
    if (how == SHUT_RD || how == SHUT_RDWR) {
        s->shut_rd = 1;
        s->stream_rx_len = 0;
    }
    if ((how == SHUT_WR || how == SHUT_RDWR) && !s->shut_wr &&
        (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)) {
        s->shut_wr = 1;
        rc = tcp_send_control(s, TCP_FLAG_ACK | TCP_FLAG_FIN);
        if (rc < 0)
            return rc;
        if (s->state == TCP_ESTABLISHED)
            s->state = TCP_FIN_WAIT1;
        else if (s->state == TCP_CLOSE_WAIT)
            s->state = TCP_LAST_ACK;
    }
    return 0;
}

int sock_close(int sid)
{
    socket_t *s = sock_get(sid);
    int i;
    if (!s)
        return -EBADF;
    for (i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].used && g_socks[i].parent_sid == sid)
            sock_reset(&g_socks[i]);
    }
    sock_reset(s);
    return 0;
}

void sock_input_udp(uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port,
                    const void *payload, size_t len)
{
    int i;
    (void)dst_ip;
    for (i = 0; i < SOCK_MAX; i++) {
        socket_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_DGRAM || !s->bound)
            continue;
        if (s->lport != dst_port)
            continue;
        if (s->laddr != 0 && s->laddr != dst_ip)
            continue;
        if (s->connected && (s->raddr != src_ip ||
                             (s->rport && s->rport != src_port)))
            continue;
        (void)rx_push(s, src_ip, src_port, payload, len);
        return;
    }
}

void sock_input_tcp(netif_t *nif, uint32_t src_ip, uint32_t dst_ip,
                    const void *segment, size_t len)
{
    const uint8_t *tcp = (const uint8_t *)segment;
    socket_t *s;
    uint16_t src_port, dst_port, flags, hdr_len;
    uint32_t seq, ack;
    size_t payload_len;
    const uint8_t *payload;

    if (!nif || !tcp || len < 20)
        return;
    src_port = (uint16_t)((tcp[0] << 8) | tcp[1]);
    dst_port = (uint16_t)((tcp[2] << 8) | tcp[3]);
    seq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) |
          ((uint32_t)tcp[6] << 8) | tcp[7];
    ack = ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16) |
          ((uint32_t)tcp[10] << 8) | tcp[11];
    hdr_len = (uint16_t)((tcp[12] >> 4) * 4);
    flags = tcp[13];
    if (hdr_len < 20 || len < hdr_len)
        return;
    payload = tcp + hdr_len;
    payload_len = len - hdr_len;

    s = tcp_find_conn(src_ip, src_port, dst_ip, dst_port);
    if (!s && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        socket_t *listener = tcp_find_listener(dst_ip, dst_port);
        int child_sid;
        socket_t *child;
        if (!listener || listener->backlog <= 0)
            return;
        child_sid = sock_alloc(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (child_sid < 0)
            return;
        child = &g_socks[child_sid];
        child->bound = 1;
        child->connected = 1;
        child->laddr = dst_ip;
        child->lport = dst_port;
        child->raddr = src_ip;
        child->rport = src_port;
        child->state = TCP_SYN_RECV;
        child->parent_sid = tcp_find_sid(listener);
        child->iss = tcp_next_iss();
        child->snd_una = child->iss;
        child->snd_nxt = child->iss;
        child->irs = seq;
        child->rcv_nxt = seq + 1;
        if (tcp_send_control(child, TCP_FLAG_SYN | TCP_FLAG_ACK) < 0)
            sock_reset(child);
        return;
    }
    if (!s)
        return;

    if (flags & TCP_FLAG_RST) {
        tcp_mark_error(s, -EPIPE);
        return;
    }

    if (s->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == s->snd_nxt) {
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            tcp_update_ack_state(s, ack);
            s->state = TCP_ESTABLISHED;
            (void)tcp_ack_now(s);
        }
        return;
    }

    if (flags & TCP_FLAG_ACK)
        tcp_update_ack_state(s, ack);

    if (payload_len > 0) {
        if (seq == s->rcv_nxt) {
            if (!s->shut_rd && stream_push(s, payload, payload_len) == 0)
                s->rcv_nxt += (uint32_t)payload_len;
            (void)tcp_ack_now(s);
        } else {
            (void)tcp_ack_now(s);
            return;
        }
    }

    if (flags & TCP_FLAG_FIN) {
        if (seq == s->rcv_nxt)
            s->rcv_nxt++;
        else if (seq + (uint32_t)payload_len == s->rcv_nxt)
            s->rcv_nxt++;
        (void)tcp_ack_now(s);
        if (s->state == TCP_ESTABLISHED)
            s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2)
            tcp_mark_error(s, 0);
    }
}

void sock_input_icmp(uint32_t src_ip, const void *payload, size_t len)
{
    int i;
    for (i = 0; i < SOCK_MAX; i++) {
        socket_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_RAW || s->proto != IPPROTO_ICMP)
            continue;
        if (s->connected && s->raddr != src_ip)
            continue;
        (void)rx_push(s, src_ip, 0, payload, len);
    }
}
