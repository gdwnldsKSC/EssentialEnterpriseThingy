/*
 * TCPIP.C  -  EssentialEnterpriseThingy v3.2
 *
 * Minimal self-contained TCP/IP stack for MS-DOS.
 *
 * Protocols implemented
 * ---------------------
 *   ARP   -- address resolution (request + reply receive)
 *   IPv4  -- send only (no fragmentation); receive for ICMP
 *   ICMP  -- echo request (ping) + echo reply receive
 *
 * All network I/O goes through the packet driver interface
 * (PKTDRV.C).  No external tools, no WATTCP, no mTCP.
 *
 * Configuration (environment variables set before running EET)
 * ------------
 *   EET_IP   = 192.168.1.100    local IP address  (required)
 *   EET_GW   = 192.168.1.1      default gateway   (optional)
 *   EET_MASK = 255.255.255.0    subnet mask       (optional, /24)
 *
 * Timer
 * -----
 * Timeouts are measured using the BIOS time-of-day counter at
 * 0040:006Ch (~18.2 ticks/second).  We read it via biostime().
 *
 * Byte order
 * ----------
 * All on-wire fields are big-endian (network order).  The host CPU
 * is little-endian (x86), so 16-bit fields are swapped with htons().
 *
 * Reference: RFC 791 (IP), RFC 792 (ICMP), RFC 826 (ARP).
 */

#include "EET.H"
#include <bios.h>       /* biostime() */

/* ---- Module state ------------------------------------------------ */

static BYTE  g_localIP[4]  = {0, 0, 0, 0};
static BYTE  g_gateway[4]  = {0, 0, 0, 0};
static BYTE  g_netmask[4]  = {255, 255, 255, 0};
static BYTE  g_localMAC[ETH_ALEN];

static BYTE  g_bcastMAC[ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static ARP_ENTRY g_arpCache[ARP_CACHE_SIZE];
static int       g_arpCount  = 0;

static BYTE      g_txBuf[14 + 20 + 1500];  /* ETH + IP + payload  */
static WORD      g_ipID    = 0x1234;
static BOOL      g_tcpipOK = FALSE;

/* Ping receive state */
static struct {
    BOOL  gotReply;
    WORD  seq;
    DWORD sendTick;
    DWORD rttTicks;
} g_ping;

/* ================================================================ */
/*  Utility                                                         */
/* ================================================================ */

/*
 * Host-to-network byte swap for a 16-bit word.
 * On little-endian x86 this reverses the two bytes.
 */
static WORD htons(WORD v)
{
    return (WORD)((v >> 8) | (v << 8));
}

#define ntohs(v)  htons(v)   /* same operation */

/*
 * IP/ICMP ones-complement 16-bit checksum.
 * Operates on an arbitrary-length byte buffer.
 */
static WORD ip_cksum(const void far *buf, WORD len)
{
    const BYTE far *p = (const BYTE far *)buf;
    DWORD           sum = 0;

    while (len > 1) {
        WORD word = (WORD)(*p) | ((WORD)(*(p + 1)) << 8);
        sum += word;
        p   += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (WORD)(*p);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (WORD)(~sum);
}

/*
 * BIOS timer ticks (0040:006Ch), ~18.2 per second.
 */
static DWORD bios_ticks(void)
{
    return biostime(0, 0L);
}

/* ================================================================ */
/*  parse_ip                                                        */
/*                                                                  */
/*  Convert dotted-decimal string to 4-byte array.                  */
/*  Returns 0 on success, -1 on bad input.                          */
/* ================================================================ */

int parse_ip(const char *str, BYTE *ip)
{
    int i;

    for (i = 0; i < 4; i++) {
        int val = 0;

        if (*str < '0' || *str > '9')
            return -1;

        while (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
            str++;
        }

        if (val > 255)
            return -1;

        ip[i] = (BYTE)val;

        if (i < 3) {
            if (*str != '.')
                return -1;
            str++;
        }
    }

    return 0;
}

/* ================================================================ */
/*  ARP cache                                                       */
/* ================================================================ */

static ARP_ENTRY *arp_lookup(const BYTE *ip)
{
    int i;
    for (i = 0; i < g_arpCount; i++) {
        if (g_arpCache[i].valid &&
            memcmp(g_arpCache[i].ip, ip, 4) == 0)
            return &g_arpCache[i];
    }
    return NULL;
}

static void arp_store(const BYTE *ip, const BYTE *mac)
{
    int i;

    for (i = 0; i < g_arpCount; i++) {
        if (memcmp(g_arpCache[i].ip, ip, 4) == 0) {
            memcpy(g_arpCache[i].mac, mac, ETH_ALEN);
            g_arpCache[i].valid = TRUE;
            return;
        }
    }

    if (g_arpCount < ARP_CACHE_SIZE) {
        memcpy(g_arpCache[g_arpCount].ip,  ip,  4);
        memcpy(g_arpCache[g_arpCount].mac, mac, ETH_ALEN);
        g_arpCache[g_arpCount].valid = TRUE;
        g_arpCount++;
    }
}

/* ================================================================ */
/*  ARP send/receive                                                */
/* ================================================================ */

static int send_arp_request(const BYTE *targetIP)
{
    BYTE       *p = g_txBuf;
    ETH_HEADER *eth;
    ARP_PACKET *arp;

    memset(g_txBuf, 0,
           sizeof(ETH_HEADER) + sizeof(ARP_PACKET));

    eth = (ETH_HEADER *)p;
    memcpy(eth->dst,  g_bcastMAC, ETH_ALEN);
    memcpy(eth->src,  g_localMAC, ETH_ALEN);
    eth->type = htons(ETH_P_ARP);
    p += sizeof(ETH_HEADER);

    arp = (ARP_PACKET *)p;
    arp->hwType    = htons(0x0001);         /* Ethernet          */
    arp->protoType = htons(ETH_P_IP);
    arp->hwLen     = ETH_ALEN;
    arp->protoLen  = 4;
    arp->op        = htons(1);              /* ARP request       */
    memcpy(arp->senderHw, g_localMAC, ETH_ALEN);
    memcpy(arp->senderIP, g_localIP,  4);
    memset(arp->targetHw, 0,          ETH_ALEN);
    memcpy(arp->targetIP, targetIP,   4);

    return pktdrv_send(g_txBuf,
                       (WORD)(sizeof(ETH_HEADER) +
                              sizeof(ARP_PACKET)));
}

static void process_arp(const BYTE far *data, WORD len)
{
    const ARP_PACKET far *arp;

    if (len < (WORD)sizeof(ARP_PACKET))
        return;

    arp = (const ARP_PACKET far *)data;

    if (ntohs(arp->op) == 2) {             /* ARP reply         */
        BYTE ip[4], mac[ETH_ALEN];
        _fmemcpy(ip,  arp->senderIP, 4);
        _fmemcpy(mac, arp->senderHw, ETH_ALEN);
        arp_store(ip, mac);
    }
}

/*
 * Send an ARP request for <ip> and wait up to 2 seconds for a
 * reply.  On success the resolved MAC is written to <mac>.
 */
static int arp_resolve(const BYTE *ip, BYTE *mac)
{
    ARP_ENTRY *entry;
    DWORD      deadline;
    void far  *rxBuf;
    WORD       rxLen;

    entry = arp_lookup(ip);
    if (entry) {
        memcpy(mac, entry->mac, ETH_ALEN);
        return ERR_OK;
    }

    send_arp_request(ip);

    deadline = bios_ticks() + (DWORD)BIOS_TICKS_SEC * 2;

    while (bios_ticks() < deadline) {
        ETH_HEADER far *eth;
        WORD            et;

        rxBuf = pktdrv_get_recv_buf(&rxLen);
        if (!rxBuf || rxLen < (WORD)sizeof(ETH_HEADER))
            continue;

        eth = (ETH_HEADER far *)rxBuf;
        et  = ntohs(eth->type);

        if (et == ETH_P_ARP)
            process_arp((const BYTE far *)rxBuf +
                        sizeof(ETH_HEADER),
                        rxLen - (WORD)sizeof(ETH_HEADER));

        entry = arp_lookup(ip);
        if (entry) {
            memcpy(mac, entry->mac, ETH_ALEN);
            return ERR_OK;
        }
    }

    return ERR_NOARP;
}

/* ================================================================ */
/*  IP send                                                         */
/* ================================================================ */

static int send_ip(const BYTE *dstIP, BYTE proto,
                   const void *payload, WORD payloadLen)
{
    BYTE        nextHop[4];
    BYTE        dstMAC[ETH_ALEN];
    BYTE       *p = g_txBuf;
    ETH_HEADER *eth;
    IP_HEADER  *ip;
    WORD        ipTotalLen;
    int         i;
    BOOL        onLink = TRUE;
    int         rc;

    /* On-link check: same network as us? */
    for (i = 0; i < 4; i++) {
        if ((dstIP[i] & g_netmask[i]) !=
            (g_localIP[i] & g_netmask[i])) {
            onLink = FALSE;
            break;
        }
    }
    memcpy(nextHop, onLink ? dstIP : g_gateway, 4);

    rc = arp_resolve(nextHop, dstMAC);
    if (rc != ERR_OK) return rc;

    ipTotalLen = (WORD)(sizeof(IP_HEADER) + payloadLen);

    /* Ethernet header */
    eth = (ETH_HEADER *)p;
    memcpy(eth->dst, dstMAC,    ETH_ALEN);
    memcpy(eth->src, g_localMAC, ETH_ALEN);
    eth->type = htons(ETH_P_IP);
    p += sizeof(ETH_HEADER);

    /* IP header */
    ip = (IP_HEADER *)p;
    ip->verIhl  = 0x45;                     /* IPv4, IHL=5 words */
    ip->tos     = 0;
    ip->len     = htons(ipTotalLen);
    ip->id      = htons(g_ipID++);
    ip->fragOff = 0;
    ip->ttl     = 64;
    ip->proto   = proto;
    ip->cksum   = 0;
    memcpy(ip->src, g_localIP, 4);
    memcpy(ip->dst, dstIP,     4);
    ip->cksum   = ip_cksum(ip, (WORD)sizeof(IP_HEADER));
    p += sizeof(IP_HEADER);

    /* Payload */
    memcpy(p, payload, payloadLen);

    return pktdrv_send(g_txBuf,
                       (WORD)(sizeof(ETH_HEADER) + ipTotalLen));
}

/* ================================================================ */
/*  ICMP echo (ping)                                                */
/* ================================================================ */

static int send_ping(const BYTE *dstIP, WORD seq)
{
    BYTE         buf[sizeof(ICMP_HEADER) + PING_DATA_LEN];
    ICMP_HEADER *icmp = (ICMP_HEADER *)buf;
    BYTE        *data = buf + sizeof(ICMP_HEADER);
    WORD         i;

    icmp->type  = ICMP_ECHO;
    icmp->code  = 0;
    icmp->cksum = 0;
    icmp->id    = htons(0xEE71);    /* "EET" identifier */
    icmp->seq   = htons(seq);

    for (i = 0; i < PING_DATA_LEN; i++)
        data[i] = (BYTE)(i & 0xFF);

    icmp->cksum = ip_cksum(buf, (WORD)sizeof(buf));

    return send_ip(dstIP, IPPROTO_ICMP,
                   buf,   (WORD)sizeof(buf));
}

static void process_icmp(const BYTE far *data, WORD len)
{
    const ICMP_HEADER far *icmp;

    if (len < (WORD)sizeof(ICMP_HEADER))
        return;

    icmp = (const ICMP_HEADER far *)data;

    if (icmp->type               == ICMP_ECHOREPLY &&
        ntohs(icmp->id)          == 0xEE71         &&
        ntohs(icmp->seq)         == g_ping.seq) {
        g_ping.gotReply = TRUE;
        g_ping.rttTicks = bios_ticks() - g_ping.sendTick;
    }
}

/* ================================================================ */
/*  Network poll -- dispatch one pending received frame             */
/* ================================================================ */

static void poll_network(void)
{
    void far       *rxBuf;
    WORD            rxLen;
    ETH_HEADER far *eth;
    WORD            et;

    rxBuf = pktdrv_get_recv_buf(&rxLen);
    if (!rxBuf || rxLen < (WORD)sizeof(ETH_HEADER))
        return;

    eth = (ETH_HEADER far *)rxBuf;
    et  = ntohs(eth->type);

    if (et == ETH_P_ARP) {
        process_arp((const BYTE far *)rxBuf +
                    sizeof(ETH_HEADER),
                    rxLen - (WORD)sizeof(ETH_HEADER));

    } else if (et == ETH_P_IP) {
        const IP_HEADER far *ip =
            (const IP_HEADER far *)((BYTE far *)rxBuf +
                                    sizeof(ETH_HEADER));
        WORD ihl;
        WORD ipPayloadLen;

        if (rxLen < (WORD)(sizeof(ETH_HEADER) +
                           sizeof(IP_HEADER)))
            return;

        if ((ip->verIhl >> 4) != 4)
            return;                         /* not IPv4          */

        if (_fmemcmp(ip->dst, g_localIP, 4) != 0)
            return;                         /* not for us        */

        ihl          = (WORD)((ip->verIhl & 0x0F) * 4);
        ipPayloadLen = ntohs(ip->len);
        if (ipPayloadLen < ihl)
            return;
        ipPayloadLen -= ihl;

        if (ip->proto == IPPROTO_ICMP)
            process_icmp((const BYTE far *)ip + ihl,
                         ipPayloadLen);
    }
}

/* ================================================================ */
/*  Public API                                                      */
/* ================================================================ */

/*
 * tcpip_init
 *
 * Read IP configuration, locate and open the packet driver, and
 * retrieve the local MAC address.  Must be called before ping_host.
 */
int tcpip_init(void)
{
    char *env;
    int   rc;

    memset(g_arpCache, 0, sizeof(g_arpCache));
    g_arpCount = 0;

    /* Read configuration from environment */
    env = getenv("EET_IP");
    if (env) parse_ip(env, g_localIP);

    env = getenv("EET_GW");
    if (env) parse_ip(env, g_gateway);

    env = getenv("EET_MASK");
    if (env) parse_ip(env, g_netmask);

    if (g_localIP[0] == 0 && g_localIP[1] == 0 &&
        g_localIP[2] == 0 && g_localIP[3] == 0) {
        fprintf(stderr,
                "EET: Set EET_IP=a.b.c.d before using /PING\n");
        return ERR_ARGS;
    }

    /* Default gateway: .1 on the same /24 if none specified */
    if (g_gateway[0] == 0) {
        memcpy(g_gateway, g_localIP, 4);
        g_gateway[3] = 1;
    }

    rc = pktdrv_find();
    if (rc != ERR_OK) {
        fprintf(stderr,
                "EET: No Crynwr packet driver found "
                "(INT 60h-7Fh)\n");
        return rc;
    }

    rc = pktdrv_open(ETH_P_IP);
    if (rc != ERR_OK) {
        fprintf(stderr, "EET: Packet driver access_type failed\n");
        return rc;
    }

    rc = pktdrv_get_address(g_localMAC);
    if (rc != ERR_OK) {
        pktdrv_close();
        fprintf(stderr, "EET: Cannot read local MAC address\n");
        return rc;
    }

    printf("  IP   : %d.%d.%d.%d\n",
           g_localIP[0], g_localIP[1],
           g_localIP[2], g_localIP[3]);
    printf("  GW   : %d.%d.%d.%d\n",
           g_gateway[0], g_gateway[1],
           g_gateway[2], g_gateway[3]);
    printf("  MAC  : %02X-%02X-%02X-%02X-%02X-%02X\n",
           g_localMAC[0], g_localMAC[1], g_localMAC[2],
           g_localMAC[3], g_localMAC[4], g_localMAC[5]);

    g_tcpipOK = TRUE;
    return ERR_OK;
}

/*
 * tcpip_shutdown  --  release the packet driver handle.
 */
void tcpip_shutdown(void)
{
    if (g_tcpipOK) {
        pktdrv_close();
        g_tcpipOK = FALSE;
    }
}

/*
 * ping_host
 *
 * Send <count> ICMP echo requests to <dstIP> and print per-packet
 * results when <verbose> is non-zero.
 * Returns ERR_OK if at least one reply was received, ERR_TIMEOUT
 * otherwise.
 */
int ping_host(const BYTE *dstIP, int count, int verbose)
{
    int   i;
    int   rxCount = 0;
    int   rc;

    for (i = 0; i < count; i++) {
        DWORD deadline;

        g_ping.gotReply = FALSE;
        g_ping.seq      = (WORD)i;
        g_ping.sendTick = bios_ticks();
        g_ping.rttTicks = 0;

        rc = send_ping(dstIP, (WORD)i);
        if (rc != ERR_OK) {
            fprintf(stderr, "EET: ICMP send failed\n");
            return rc;
        }

        deadline = bios_ticks() +
                   (DWORD)BIOS_TICKS_SEC * PING_TIMEOUT_SEC;

        while (!g_ping.gotReply && bios_ticks() < deadline)
            poll_network();

        if (g_ping.gotReply) {
            DWORD rttMs = (g_ping.rttTicks * 1000UL)
                        / (DWORD)BIOS_TICKS_SEC;
            if (verbose)
                printf("  Reply from %d.%d.%d.%d: "
                       "seq=%u time=%lums\n",
                       dstIP[0], dstIP[1], dstIP[2], dstIP[3],
                       (unsigned)i, rttMs);
            rxCount++;
        } else {
            if (verbose)
                printf("  Request timeout for seq %u\n",
                       (unsigned)i);
        }
    }

    printf("  %d/%d packets received.\n", rxCount, count);
    return (rxCount > 0) ? ERR_OK : ERR_TIMEOUT;
}
