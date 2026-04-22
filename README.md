# EssentialEnterpriseThingy
"Essential expansion LanDesk native executable command batching DriveMapper and disc defragmenter v3.2 - just what you've always wanted." 

This repository now includes a minimal **MS-DOS compatible** batch utility with optional **TCP/IP** checks:

- `EET.BAT` - command batching wrapper for:
  - drive mapping (`NET USE`)
  - disk defragmentation (`DEFRAG`)
  - TCP/IP connectivity checks (`PING` or `\MTCP\PING.EXE`)

## Usage

Run one or more operations in a single command (batched in order):

```bat
EET.BAT /MAP F: \\SERVER\TOOLS /DEFRAG C: /TCPIP 192.168.1.1
```

Use a single switch:

```bat
EET.BAT /DEFRAG C:
```

Use `/ALL` with environment variables:

```bat
SET MAP_DRIVE=F:
SET SHARE_PATH=\\SERVER\TOOLS
SET DEFRAG_DRIVE=C:
SET TCP_HOST=192.168.1.1
EET.BAT /ALL
```
