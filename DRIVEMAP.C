/*
 * DRIVEMAP.C  -  EssentialEnterpriseThingy v3.2
 *
 * Network drive mapping via the DOS Network Redirector.
 *
 * INT 21h  AH=5Fh  AL=03h  -- Create device redirection (map drive)
 * INT 21h  AH=5Fh  AL=04h  -- Cancel device redirection (unmap drive)
 *
 * Requires that a DOS network redirector is already loaded:
 *   - Microsoft Network Client 3.0 (NET.EXE + WORKGRP.SYS)
 *   - LAN Manager 2.x (REDIR.EXE)
 *   - Novell NetWare shell (NETX.EXE / VLM)
 *   - LanDesk OS/2 redirector
 *
 * Redirector presence is detected via INT 2Fh AX=1100h (IFSFUNC
 * installed check) and, as a fallback, AX=B800h (NetWare shell).
 *
 * Reference: "MS-DOS Programmer's Reference", Microsoft Press 1991;
 *            Ralf Brown's Interrupt List.
 */

#include "EET.H"

/* ---------------------------------------------------------------- */
/*  redirector_present                                              */
/*                                                                  */
/*  Returns TRUE if a DOS network redirector is installed.          */
/*  Checks INT 2Fh AX=1100h (Microsoft IFSFUNC) first, then        */
/*  AX=B800h (Novell NetWare shell).                                */
/* ---------------------------------------------------------------- */

BOOL redirector_present(void)
{
    union  REGS  regs;
    struct SREGS segs;

    /* Microsoft IFSFUNC / network redirector installed query.
     * If AL returns 0xFF on exit the redirector is present.       */
    regs.x.ax = 0x1100;
    int86x(0x2F, &regs, &regs, &segs);
    if (regs.h.al == 0xFF)
        return TRUE;

    /* Fallback: NetWare shell presence query.
     * BH != 0 on return means NetWare shell is loaded.            */
    regs.x.ax = 0xB800;
    int86x(0x2F, &regs, &regs, &segs);
    if (regs.h.bh != 0x00)
        return TRUE;

    return FALSE;
}

/* ---------------------------------------------------------------- */
/*  map_network_drive                                               */
/*                                                                  */
/*  Maps <driveLetter>: to the UNC path <uncPath>.                  */
/*  <password> may be NULL or empty for no-password shares.         */
/*                                                                  */
/*  INT 21h  AH=5Fh  AL=03h                                        */
/*    BX  = device type  (0004h = disk drive)                       */
/*    CX  = user data word (0 = none)                               */
/*    DS:SI -> local device name  e.g. "F:\0"                       */
/*    ES:DI -> network path       e.g. "\\SERVER\SHARE\0"           */
/*             (some redirectors also want a password appended       */
/*              after a NUL byte; we pass it in separately as CX     */
/*              is sometimes overloaded -- redirector-dependent)     */
/*                                                                  */
/*  Returns ERR_OK on success, or the DOS error code on failure.    */
/* ---------------------------------------------------------------- */

int map_network_drive(char driveLetter, const char *uncPath,
                      const char *password)
{
    union  REGS  regs;
    struct SREGS segs;
    char   localDev[4];
    char   netPath[130];    /* UNC + optional NUL + password       */
    int    uncLen;

    /* Build "X:" local device string */
    localDev[0] = (char)toupper((unsigned char)driveLetter);
    localDev[1] = ':';
    localDev[2] = '\0';

    /* Build network path.  Some redirectors expect the password
     * appended after a second NUL byte (LAN Manager style).       */
    uncLen = (int)strlen(uncPath);
    if (uncLen > 127) uncLen = 127;
    memcpy(netPath, uncPath, (unsigned)uncLen);
    netPath[uncLen] = '\0';

    if (password && *password) {
        int passLen = (int)strlen(password);
        if (uncLen + 1 + passLen < 129) {
            memcpy(netPath + uncLen + 1, password, (unsigned)passLen);
            netPath[uncLen + 1 + passLen] = '\0';
        }
    }

    regs.h.ah = 0x5F;
    regs.h.al = 0x03;
    regs.x.bx = REDIR_DISK;
    regs.x.cx = 0x0000;
    segs.ds   = FP_SEG(localDev);
    regs.x.si = FP_OFF(localDev);
    segs.es   = FP_SEG(netPath);
    regs.x.di = FP_OFF(netPath);

    int86x(0x21, &regs, &regs, &segs);

    if (regs.x.cflag)
        return (int)regs.x.ax;     /* DOS extended error code */

    return ERR_OK;
}

/* ---------------------------------------------------------------- */
/*  unmap_network_drive                                             */
/*                                                                  */
/*  Cancels an existing drive redirection.                          */
/*                                                                  */
/*  INT 21h  AH=5Fh  AL=04h                                        */
/*    DS:SI -> local device name  e.g. "F:\0"                       */
/* ---------------------------------------------------------------- */

int unmap_network_drive(char driveLetter)
{
    union  REGS  regs;
    struct SREGS segs;
    char   localDev[4];

    localDev[0] = (char)toupper((unsigned char)driveLetter);
    localDev[1] = ':';
    localDev[2] = '\0';

    regs.h.ah = 0x5F;
    regs.h.al = 0x04;
    segs.ds   = FP_SEG(localDev);
    regs.x.si = FP_OFF(localDev);

    int86x(0x21, &regs, &regs, &segs);

    if (regs.x.cflag)
        return (int)regs.x.ax;

    return ERR_OK;
}
