# EssentialEnterpriseThingy
"Essential expansion LanDesk native executable command batching DriveMapper and disc defragmenter v3.2 - just what you've always wanted."

A genuine **16-bit MS-DOS utility** (circa 1998).  No external tools.
No `NET USE`, no `DEFRAG.EXE`, no `PING.EXE` — `EET.EXE` implements
everything itself using direct DOS and BIOS interrupts.

## Features

| Feature | How it works |
|---------|-------------|
| Network drive mapping | INT 21h AH=5Fh AL=03h/04h (DOS Network Redirector) |
| Drive unmapping | INT 21h AH=5Fh AL=04h |
| Disk defragmentation | FAT12/FAT16 via INT 25h/26h absolute sector I/O |
| TCP/IP ping | ARP + ICMP over a Crynwr packet driver (INT 60h–7Fh) |
| Command batching | Multiple operations on one command line or in a file |

## Requirements

- **MS-DOS 5.0** or later
- **Drive mapping:** a DOS network redirector must be loaded before
  running EET (e.g. Microsoft Network Client 3.0, LAN Manager 2.x,
  NetWare NETX/VLM, or LanDesk REDIR)
- **TCP/IP:** a Crynwr-compatible packet driver must be loaded for
  your Ethernet card (e.g. `NE2000 0x60 5 0x300`), and these
  environment variables must be set:

  ```
  SET EET_IP=192.168.1.100
  SET EET_GW=192.168.1.1
  SET EET_MASK=255.255.255.0
  ```

## Building

Requires **Borland C++ 3.1** or **Watcom C/C++ 10.x** and DOS MAKE.

```
MAKE              (Borland C++ 3.1)
MAKE WATCOM=1     (Watcom C/C++ 10.x)
```

Output: `EET.EXE` — a single self-contained 16-bit DOS executable.

## Usage

```
EET /MAP    <drive:> <\\server\share> [password]
EET /UNMAP  <drive:>
EET /DEFRAG <drive:> [/V]
EET /PING   <a.b.c.d> [count]
EET /BATCH  <file.txt>
EET /?
```

Switches may be **chained** on a single command line (executed in order):

```
EET /MAP F: \\LANDESK\DIST /DEFRAG C: /PING 192.168.1.1 4
```

`/BATCH` reads a plain text file with one set of switches per line;
lines beginning with `;` are comments:

```
; EET batch file
/MAP    F: \\LANDESK\TOOLS
/DEFRAG C: /V
/PING   192.168.1.1
```

## Source files

| File | Description |
|------|-------------|
| `EET.C` | Main entry point, argument parser, batch execution loop |
| `EET.H` | Shared types, constants, packed structs, prototypes |
| `DRIVEMAP.C` | Network drive map/unmap (INT 21h/5Fh) |
| `DEFRAG.C` | FAT12/FAT16 defragmenter (INT 25h/26h) |
| `TCPIP.C` | ARP, IPv4, ICMP echo — built-in TCP/IP stack |
| `PKTDRV.C` | Crynwr packet driver interface (INT 60h–7Fh) |
| `MAKEFILE` | Borland C++ 3.1 / Watcom C 10.x build rules |
