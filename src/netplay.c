/* model3recomp -- two-player netplay, lockstep over TCP. See netplay.h. */
#include "model3recomp/netplay.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netdb.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
#endif

#define NP_VERSION   1u
#define NP_MSG       24
#define NP_RING      256u       /* fields of remote input held */
#define NP_HASH_EVERY 120u      /* about two seconds */
#define NP_TIMEOUT_MS 15000

enum { MSG_HELLO = 1, MSG_INPUT = 2, MSG_HASH = 3, MSG_NVRAM = 4, MSG_BYE = 5 };

static sock_t   g_sock = BAD_SOCK;
static int      g_role;             /* 0 none, 1 host, 2 joiner */
static unsigned g_delay = 3;
static char     g_status[128];
static int      g_desync;
static uint64_t g_first = UINT64_MAX;   /* the first field traded */

static struct { uint64_t f; int valid; m3_input_t in; } g_remote[NP_RING];
static struct { uint64_t f; uint32_t h; int valid; } g_hash_mine, g_hash_peer;

int netplay_active(void) { return g_role != 0; }
int netplay_role(void) { return g_role; }
const char *netplay_status(void) { return g_status; }

/* ---- wire format: 24 bytes, little-endian -------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int send_all(const uint8_t *p, size_t n)
{
    while (n) {
        int k = send(g_sock, (const char *)p, (int)n, 0);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

static int send_msg(unsigned type, uint64_t f, uint32_t a, uint32_t b,
                    uint32_t c, uint32_t d)
{
    uint8_t m[NP_MSG];
    memset(m, 0, sizeof m);
    m[0] = (uint8_t)type;
    put32(m + 4, (uint32_t)f);
    put32(m + 8, a); put32(m + 12, b); put32(m + 16, c); put32(m + 20, d);
    return send_all(m, sizeof m);
}

static int send_input(uint64_t f, const m3_input_t *in)
{
    return send_msg(MSG_INPUT, f, in->buttons,
                    (uint32_t)in->gun_x[0] | ((uint32_t)in->gun_y[0] << 16),
                    in->cheats, in->options);
}

/* Wait up to ms for the socket to be readable, keeping the window alive. */
static int wait_readable(int ms)
{
    fd_set rs;
    struct timeval tv;
    FD_ZERO(&rs);
    FD_SET(g_sock, &rs);
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    return select((int)g_sock + 1, &rs, NULL, NULL, &tv) > 0;
}

static int recv_all(uint8_t *p, size_t n)
{
    int waited = 0;
    while (n) {
        int k;
        if (!wait_readable(20)) {
            if (!platform_poll()) { netplay_shutdown(); exit(0); }
            waited += 20;
            if (waited > NP_TIMEOUT_MS) return 0;
            continue;
        }
        k = recv(g_sock, (char *)p, (int)n, 0);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k; waited = 0;
    }
    return 1;
}

static uint32_t ram_hash(void)
{
    const uint8_t *r = bus_ram();
    uint64_t h = 1469598103934665603ull, w;
    size_t i;
    if (!r) return 0;
    for (i = 0; i + 8 <= M3_RAM_SIZE; i += 8) {
        memcpy(&w, r + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static void lost(const char *why)
{
    fprintf(stderr, "[netplay] %s -- playing on alone\n", why);
    snprintf(g_status, sizeof g_status, "netplay: %s", why);
    platform_set_status(g_status);
    if (g_sock != BAD_SOCK) sock_close(g_sock);
    g_sock = BAD_SOCK;
    g_role = 0;
}

/* Read one message and file it. 0 on a dead connection. */
static int pump_one(void)
{
    uint8_t m[NP_MSG];
    uint64_t f;
    if (!recv_all(m, sizeof m)) return 0;
    f = get32(m + 4);
    switch (m[0]) {
    case MSG_INPUT: {
        unsigned k = (unsigned)(f % NP_RING);
        uint32_t g = get32(m + 12);
        g_remote[k].f = f;
        g_remote[k].valid = 1;
        g_remote[k].in.buttons = get32(m + 8);
        g_remote[k].in.gun_x[0] = (uint16_t)(g & 0xFFFFu);
        g_remote[k].in.gun_y[0] = (uint16_t)(g >> 16);
        g_remote[k].in.cheats = get32(m + 16);
        g_remote[k].in.options = get32(m + 20);
        break;
    }
    case MSG_HASH:
        g_hash_peer.f = f; g_hash_peer.h = get32(m + 8); g_hash_peer.valid = 1;
        break;
    case MSG_BYE:
        return 0;
    default:
        break;
    }
    return 1;
}

static void check_hashes(void)
{
    if (!g_hash_mine.valid || !g_hash_peer.valid || g_hash_mine.f != g_hash_peer.f)
        return;
    if (g_hash_mine.h != g_hash_peer.h && !g_desync) {
        g_desync = 1;
        fprintf(stderr, "[netplay] DESYNC at field %llu: %08X here, %08X there\n",
                (unsigned long long)g_hash_mine.f, g_hash_mine.h, g_hash_peer.h);
        snprintf(g_status, sizeof g_status, "netplay: DESYNC at field %llu",
                 (unsigned long long)g_hash_mine.f);
        platform_set_status(g_status);
    }
    g_hash_peer.valid = 0;
}

/* Player 1's controls, moved over to player 2's. */
static uint32_t as_p2(uint32_t b)
{
    uint32_t o = 0;
    if (b & M3_BTN_COIN1)   o |= M3_BTN_COIN2;
    if (b & M3_BTN_START1)  o |= M3_BTN_START2;
    if (b & M3_BTN_TRIG1)   o |= M3_BTN_TRIG2;
    if (b & M3_BTN_OFFSCR1) o |= M3_BTN_OFFSCR2;
    return o;
}

#define P1_BITS (M3_BTN_COIN1 | M3_BTN_START1 | M3_BTN_TRIG1 | M3_BTN_OFFSCR1)

static void merge(const m3_input_t *host, const m3_input_t *join, m3_input_t *out)
{
    memset(out, 0, sizeof *out);
    out->buttons = (host->buttons & (P1_BITS | M3_BTN_TEST | M3_BTN_SERVICE))
                 | as_p2(join->buttons);
    out->gun_x[0] = host->gun_x[0]; out->gun_y[0] = host->gun_y[0];
    out->gun_x[1] = join->gun_x[0]; out->gun_y[1] = join->gun_y[0];
    out->cheats = host->cheats;
    out->options = host->options;
}

static void neutral(m3_input_t *in)
{
    memset(in, 0, sizeof *in);
    in->gun_x[0] = in->gun_x[1] = (M3_GUN_X0 + M3_GUN_X1) / 2;
    in->gun_y[0] = in->gun_y[1] = (M3_GUN_Y0 + M3_GUN_Y1) / 2;
}

int netplay_exchange(const m3_input_t *local, m3_input_t *out, uint64_t f)
{
    static m3_input_t mine[NP_RING];
    m3_input_t remote, me;
    unsigned k;

    if (!g_role) return 0;
    if (g_first == UINT64_MAX) g_first = f;   /* both sides start on the same field */

    /* Mine for f + delay goes out now; the first `delay` fields of the
     * session are neutral on both sides, which is what lets the pipe fill. */
    mine[(f + g_delay) % NP_RING] = *local;
    if (!send_input(f + g_delay, local)) { lost("peer went away"); return 0; }

    if (f % NP_HASH_EVERY == 0 && f) {
        g_hash_mine.f = f; g_hash_mine.h = ram_hash(); g_hash_mine.valid = 1;
        /* In the log too, every ten seconds, so a harness can set the two
         * machines' logs side by side. */
        if (f % (NP_HASH_EVERY * 5u) == 0)
            fprintf(stderr, "[netplay] field %llu ram %08X\n",
                    (unsigned long long)f, g_hash_mine.h);
        send_msg(MSG_HASH, f, g_hash_mine.h, 0, 0, 0);
    }

    if (f < g_first + g_delay) {
        neutral(&me); neutral(&remote);
    } else {
        k = (unsigned)(f % NP_RING);
        while (!(g_remote[k].valid && g_remote[k].f == f))
            if (!pump_one()) { lost("peer went away"); return 0; }
        remote = g_remote[k].in;
        g_remote[k].valid = 0;
        me = mine[k];
    }
    check_hashes();

    if (g_role == 1) merge(&me, &remote, out);
    else             merge(&remote, &me, out);
    return 1;
}

/* ---- connecting ------------------------------------------------------- */
static int net_up(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return 1;
#endif
}

static void tune(sock_t s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static sock_t listen_on(const char *port)
{
    struct addrinfo hints, *ai = NULL;
    sock_t ls, s = BAD_SOCK;
    int one = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;           /* dual-stack: v4 and v6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &ai) != 0) {
        hints.ai_family = AF_INET;
        if (getaddrinfo(NULL, port, &hints, &ai) != 0) return BAD_SOCK;
    }
    ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (ls == BAD_SOCK) { freeaddrinfo(ai); return BAD_SOCK; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (ai->ai_family == AF_INET6) {
        int zero = 0;
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
    }
    if (bind(ls, ai->ai_addr, (int)ai->ai_addrlen) != 0 || listen(ls, 1) != 0) {
        freeaddrinfo(ai); sock_close(ls); return BAD_SOCK;
    }
    freeaddrinfo(ai);

    snprintf(g_status, sizeof g_status, "netplay: hosting on port %s, waiting for player 2", port);
    platform_set_status(g_status);
    fprintf(stderr, "[netplay] %s\n", g_status);
    for (;;) {
        fd_set rs;
        struct timeval tv = { 0, 50000 };
        FD_ZERO(&rs); FD_SET(ls, &rs);
        if (select((int)ls + 1, &rs, NULL, NULL, &tv) > 0) {
            s = accept(ls, NULL, NULL);
            break;
        }
        if (!platform_poll()) { sock_close(ls); exit(0); }
    }
    sock_close(ls);
    return s;
}

static sock_t connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *ai = NULL, *p;
    int tries;
    snprintf(g_status, sizeof g_status, "netplay: joining %s:%s", host, port);
    platform_set_status(g_status);
    fprintf(stderr, "[netplay] %s\n", g_status);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    /* The host may still be starting up: keep trying for half a minute. */
    for (tries = 0; tries < 60; tries++) {
        if (getaddrinfo(host, port, &hints, &ai) == 0) {
            for (p = ai; p; p = p->ai_next) {
                sock_t s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (s == BAD_SOCK) continue;
                if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) {
                    freeaddrinfo(ai);
                    return s;
                }
                sock_close(s);
            }
            freeaddrinfo(ai);
        }
        {
            int i;
            for (i = 0; i < 10; i++) {
                if (!platform_poll()) exit(0);
#ifdef _WIN32
                Sleep(50);
#else
                usleep(50000);
#endif
            }
        }
    }
    return BAD_SOCK;
}

/* The joiner takes the host's battery-backed RAM and EEPROM: high scores
 * and settings, the country among them, are guest state, and two machines that differ there differ everywhere. */
static int trade_nvram(void)
{
    size_t n = 0;
    uint8_t *b = bus_backup(&n);
    if (g_role == 1) {
        if (!send_msg(MSG_NVRAM, 0, (uint32_t)n, 0, 0, 0)) return 0;
        return (n ? send_all(b, n) : 1) && send_all((const uint8_t *)bus_eeprom(), 128);
    } else {
        uint8_t m[NP_MSG];
        uint32_t len;
        if (!recv_all(m, sizeof m) || m[0] != MSG_NVRAM) return 0;
        len = get32(m + 8);
        if (len != n) return 0;
        return (n ? recv_all(b, n) : 1) && recv_all((uint8_t *)bus_eeprom(), 128);
    }
}

int netplay_init_from_env(void)
{
    const char *e = getenv("M3_NETPLAY");
    char buf[256], *host, *port;
    uint8_t m[NP_MSG];

    if (!e || !*e) return 1;
    snprintf(buf, sizeof buf, "%s", e);
    if (!net_up()) return 0;
    if (getenv("M3_NET_DELAY")) g_delay = (unsigned)strtoul(getenv("M3_NET_DELAY"), NULL, 0);
    if (g_delay < 1) g_delay = 1;
    if (g_delay > 30) g_delay = 30;

    if (!strncmp(buf, "host:", 5)) {
        g_role = 1;
        g_sock = listen_on(buf + 5);
    } else if (!strncmp(buf, "join:", 5)) {
        g_role = 2;
        host = buf + 5;
        port = strrchr(host, ':');
        if (!port) { fprintf(stderr, "[netplay] M3_NETPLAY=join:ADDR:PORT\n"); g_role = 0; return 0; }
        *port++ = 0;
        if (*host == '[') {             /* [v6]:port */
            host++;
            if (host[strlen(host) - 1] == ']') host[strlen(host) - 1] = 0;
        }
        g_sock = connect_to(host, port);
    } else {
        fprintf(stderr, "[netplay] M3_NETPLAY must be host:PORT or join:ADDR:PORT\n");
        return 0;
    }
    if (g_sock == BAD_SOCK) { lost("could not connect"); return 0; }
    tune(g_sock);

    /* Hello both ways: version, role, and the host's input delay. */
    if (!send_msg(MSG_HELLO, NP_VERSION, (uint32_t)g_role, g_delay, 0, 0)
        || !recv_all(m, sizeof m) || m[0] != MSG_HELLO) {
        lost("handshake failed");
        return 0;
    }
    if (get32(m + 4) != NP_VERSION) { lost("the other side is a different version"); return 0; }
    if ((int)get32(m + 8) == g_role) { lost("both sides chose the same role"); return 0; }
    if (g_role == 2) g_delay = get32(m + 12);
    if (!trade_nvram()) { lost("could not share battery RAM"); return 0; }

    snprintf(g_status, sizeof g_status, "netplay: %s, delay %u",
             g_role == 1 ? "host (player 1)" : "joined (player 2)", g_delay);
    platform_set_status(g_status);
    fprintf(stderr, "[netplay] %s\n", g_status);
    return 1;
}

void netplay_shutdown(void)
{
    if (g_sock != BAD_SOCK) {
        send_msg(MSG_BYE, 0, 0, 0, 0, 0);
        sock_close(g_sock);
    }
    g_sock = BAD_SOCK;
    g_role = 0;
}
