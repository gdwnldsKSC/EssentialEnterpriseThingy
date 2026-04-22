/*
 * EET.H  -  EssentialEnterpriseThingy v3.2
 *
 * Shared types, constants, structure definitions and function prototypes.
 * Target: MS-DOS 5.0+, 16-bit real mode, large memory model.
 * Compiler: Borland C++ 3.1  -or-  Watcom C/C++ 10.x
 */

#ifndef EET_H
#define EET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <mem.h>

/* ---- Portable DOS types ----------------------------------------- */

typedef unsigned char   BYTE;
typedef unsigned int    WORD;
typedef unsigned long   DWORD;
typedef int             BOOL;

#ifndef TRUE
#  define TRUE  1
#  define FALSE 0
#endif

/* ---- Drive-mapping constants ------------------------------------ */

#define REDIR_DISK       0x0004   /* DOS redirector device type: disk */
#define MAX_DRIVES       26

/* ---- Defrag constants ------------------------------------------ */

#define SECTOR_SIZE      512
#define MAX_CLUSTERS     65524    /* FAT16 upper bound               */
#define FAT_FREE         0x0000
#define FAT_BAD          0xFFF7
#define FAT_EOC          0xFFF8   /* End-of-chain threshold          */
#define MAX_FILES        512      /* root-dir entries tracked        */

/* ---- TCP/IP / packet-driver constants -------------------------- */

#define ETH_ALEN         6
#define ETH_P_IP         0x0800
#define ETH_P_ARP        0x0806
#define IPPROTO_ICMP     1
#define ICMP_ECHO        8
#define ICMP_ECHOREPLY   0
#define ARP_CACHE_SIZE   16
#define PING_TIMEOUT_SEC 3        /* seconds per echo               */
#define PING_DATA_LEN    32       /* ICMP data payload bytes        */
#define BIOS_TICKS_SEC   18       /* ~18.2 timer ticks per second   */

/* Crynwr packet-driver interrupt scan range */
#define PKT_INT_FIRST    0x60
#define PKT_INT_LAST     0x80
#define PKT_CLASS_ETHER  1        /* interface class: Ethernet      */

/* ---- Error codes ----------------------------------------------- */

#define ERR_OK           0
#define ERR_ARGS        -1
#define ERR_NODRIVE     -2
#define ERR_NOREDIR     -3
#define ERR_NOFAT       -4
#define ERR_IO          -5
#define ERR_NOPKTDRV    -6
#define ERR_NOARP       -7
#define ERR_TIMEOUT     -8
#define ERR_NOMEM       -9

/* ================================================================ */
/*  On-disk structures (packed, no padding)                         */
/* ================================================================ */

#pragma pack(1)

/* FAT12/FAT16 boot sector (BPB fields we care about) */
typedef struct {
    BYTE  jmpBoot[3];
    BYTE  oemName[8];
    WORD  bytesPerSec;
    BYTE  secPerClust;
    WORD  rsvdSecCnt;
    BYTE  numFATs;
    WORD  rootEntCnt;
    WORD  totSec16;
    BYTE  media;
    WORD  fatSz16;
    WORD  secPerTrk;
    WORD  numHeads;
    DWORD hiddSec;
    DWORD totSec32;
    BYTE  drvNum;
    BYTE  reserved1;
    BYTE  bootSig;
    DWORD volID;
    BYTE  volLab[11];
    BYTE  filSysType[8];
} FAT_BOOT_SECTOR;

/* FAT 8.3 directory entry */
typedef struct {
    BYTE  name[8];
    BYTE  ext[3];
    BYTE  attr;
    BYTE  reserved[10];
    WORD  wrtTime;
    WORD  wrtDate;
    WORD  fstClus;
    DWORD fileSize;
} FAT_DIR_ENTRY;

/* Ethernet II frame header */
typedef struct {
    BYTE  dst[ETH_ALEN];
    BYTE  src[ETH_ALEN];
    WORD  type;             /* network byte order */
} ETH_HEADER;

/* ARP packet (Ethernet + IPv4) */
typedef struct {
    WORD  hwType;           /* 0x0001 = Ethernet (network order) */
    WORD  protoType;        /* 0x0800 = IPv4    (network order) */
    BYTE  hwLen;
    BYTE  protoLen;
    WORD  op;               /* 1=request 2=reply (network order) */
    BYTE  senderHw[ETH_ALEN];
    BYTE  senderIP[4];
    BYTE  targetHw[ETH_ALEN];
    BYTE  targetIP[4];
} ARP_PACKET;

/* IPv4 header (no options) */
typedef struct {
    BYTE  verIhl;           /* version(4)|IHL(4) */
    BYTE  tos;
    WORD  len;              /* network byte order */
    WORD  id;
    WORD  fragOff;
    BYTE  ttl;
    BYTE  proto;
    WORD  cksum;
    BYTE  src[4];
    BYTE  dst[4];
} IP_HEADER;

/* ICMP header */
typedef struct {
    BYTE  type;
    BYTE  code;
    WORD  cksum;
    WORD  id;
    WORD  seq;
} ICMP_HEADER;

#pragma pack()

/* ================================================================ */
/*  In-memory structures                                            */
/* ================================================================ */

/* Volume geometry used by the defragmenter */
typedef struct {
    BYTE  driveNum;         /* 0=A, 1=B, 2=C ... */
    WORD  bytesPerSec;
    BYTE  secPerClust;
    WORD  rsvdSecCnt;
    BYTE  numFATs;
    WORD  rootEntCnt;
    WORD  fatSz16;
    DWORD rootDirSec;       /* LBA of first root-directory sector  */
    DWORD firstDataSec;     /* LBA of first cluster-2 data sector  */
    WORD  totalClusters;
    WORD  *fat;             /* heap-allocated FAT table (farmalloc) */
} VOLUME_INFO;

/* ARP cache entry */
typedef struct {
    BYTE  ip[4];
    BYTE  mac[ETH_ALEN];
    BOOL  valid;
} ARP_ENTRY;

/* ================================================================ */
/*  Function prototypes                                             */
/* ================================================================ */

/* --- drivemap.c --- */
BOOL redirector_present(void);
int  map_network_drive(char driveLetter, const char *uncPath,
                       const char *password);
int  unmap_network_drive(char driveLetter);

/* --- defrag.c --- */
int  defrag_volume(char driveLetter, BOOL verbose);

/* --- tcpip.c --- */
int  parse_ip(const char *str, BYTE *ip);
int  tcpip_init(void);
void tcpip_shutdown(void);
int  ping_host(const BYTE *dstIP, int count, int verbose);

/* --- pktdrv.c --- */
int       pktdrv_find(void);
int       pktdrv_open(WORD pktType);
void      pktdrv_close(void);
int       pktdrv_send(const void far *buf, WORD len);
int       pktdrv_get_address(BYTE *mac);
void far *pktdrv_get_recv_buf(WORD *len);

#endif /* EET_H */
