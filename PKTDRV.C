/*
 * PKTDRV.C  -  EssentialEnterpriseThingy v3.2
 *
 * Interface to a Crynwr-compatible DOS packet driver.
 *
 * The packet driver specification (Crynwr, 1990) defines a set of
 * functions invoked through a software interrupt (typically INT 60h
 * through INT 7Fh).  A loaded driver identifies itself by placing
 * the 8-byte ASCII string "PKT DRVR" at offset 3 from the start of
 * its ISR.  We scan the interrupt table to find it.
 *
 * Functions used
 * --------------
 *  AX=0001h  driver_info    -- query driver type and version
 *  AX=0002h  access_type    -- register a packet-type receiver
 *  AX=0003h  release_type   -- unregister receiver
 *  AX=0004h  send_pkt       -- transmit a raw frame
 *  AX=0006h  get_address    -- retrieve the local hardware address
 *
 * Receiver upcall
 * ---------------
 * When the driver receives a frame whose Ethertype matches the one
 * we registered with access_type, it calls our "receiver" routine
 * twice:
 *
 *   First call  (AX=0000h): BX=handle, CX=frame length.
 *     We must return ES:DI pointing to a buffer of at least CX bytes.
 *
 *   Second call (AX=0001h): data has been copied into our buffer.
 *     We set g_rxReady and g_rxLen so that poll callers see it.
 *
 * Because the driver calls our routine from an interrupt context with
 * DS pointing to the driver's own data segment, the receiver function
 * is declared far and its prologue must not assume DS.  In Borland
 * C++ this is achieved with the "interrupt" or "far" qualifier plus
 * explicit DS save/restore via inline assembly.  The entry point
 * below is written in pure inline assembly for correctness.
 *
 * Reference: Crynwr Packet Driver Specification, Rev 11, 1994.
 *            Ralf Brown's Interrupt List, INT 60h entry.
 */

#include "EET.H"

/* ---- Module state ------------------------------------------------ */

static int   g_pktInt  = -1;       /* INT vector where driver lives  */
static WORD  g_handle  = 0;        /* handle from access_type        */

static BYTE  g_rxBuf[1514];        /* max Ethernet frame             */
static WORD  g_rxLen   = 0;
static BOOL  g_rxReady = FALSE;

/* ================================================================ */
/*  Receiver upcall (called from interrupt context by packet driver)*/
/* ================================================================ */

/*
 * The packet driver calls this function twice per received frame.
 *
 * We store our buffer address in g_rxBufPtr; on the first call
 * (AX=0) we return ES:DI = g_rxBuf; on the second call (AX=1) we
 * record the frame length and signal g_rxReady.
 *
 * Borland C++ inline assembly:  _asm { ... }
 * The function is declared void far receiver(void) but we never
 * call it from C -- its address is passed to the packet driver via
 * access_type.  Registers on entry:
 *   AX = 0 (buffer request) or 1 (copy done)
 *   BX = handle
 *   CX = length of received frame
 */
static void far receiver(void)
{
#if defined(__TURBOC__) || defined(__BORLANDC__)
    _asm {
        /*
         * The packet driver calls this routine from its ISR.
         * Registers on entry:
         *   AX = 0  --> driver wants a receive buffer; return ES:DI
         *   AX = 1  --> data has been placed in our buffer (CX=len)
         *   BX = handle, CX = frame length
         *
         * We save every register we touch (AX included via BX below)
         * and restore them before returning.  DS is saved first so
         * we can reload our own data segment for globals access; the
         * original AX value (call type 0 or 1) is preserved in BX
         * across the DS reload.
         */
        push bx
        push cx
        push dx
        push si
        push ds

        /* Preserve the call-type flag (AX) in SI before we need AX
         * for segment arithmetic.                                    */
        mov  si, ax

        /* Load our module's data segment */
        mov  ax, seg g_rxBuf
        mov  ds, ax

        /* Branch on call type (0 = provide buffer, 1 = data ready) */
        or   si, si
        jnz  second_call

        /* --- First call (AX=0): return buffer address in ES:DI --- */
        mov  ax, seg g_rxBuf
        mov  es, ax
        lea  di, g_rxBuf
        jmp  recv_done

        /* --- Second call (AX=1): record received length, signal -- */
    second_call:
        mov  g_rxLen,   cx
        mov  g_rxReady, 1

    recv_done:
        pop  ds
        pop  si
        pop  dx
        pop  cx
        pop  bx
    }
#endif
    /* Watcom C: receiver must be implemented as a separate .ASM
     * module using the __cdecl far calling convention.            */
}

/* ================================================================ */
/*  pktdrv_find                                                     */
/*                                                                  */
/*  Scan INT 60h..7Fh for the "PKT DRVR" signature.                */
/*  On success g_pktInt is set and ERR_OK is returned.             */
/* ================================================================ */

int pktdrv_find(void)
{
    int      intVec;
    BYTE far *isr;

    for (intVec = PKT_INT_FIRST; intVec < PKT_INT_LAST; intVec++) {

        /* Retrieve the ISR far pointer from the DOS IVT */
        isr = (BYTE far *)MK_FP(
                  (WORD)((unsigned long)getvect(intVec) >> 16),
                  (WORD)((unsigned long)getvect(intVec) & 0xFFFFUL));

        if (isr == NULL)
            continue;

        /*
         * Crynwr spec: the packet-driver ISR begins with a FAR JMP
         * or NOP sequence; "PKT DRVR" appears at byte offset 3.
         */
        if (_fmemcmp(isr + 3, "PKT DRVR", 8) == 0) {
            g_pktInt = intVec;
            return ERR_OK;
        }
    }

    return ERR_NOPKTDRV;
}

/* ================================================================ */
/*  pktdrv_open   (access_type, AX=0002h)                          */
/*                                                                  */
/*  Register our receiver for Ethernet frames of type <pktType>.   */
/*  BX = interface class (1 = Ethernet)                             */
/*  DX = interface number (0xFFFF = any)                            */
/*  CX = length of type descriptor (2 bytes for Ethertype)         */
/*  DS:SI -> Ethertype value (network byte order)                   */
/*  ES:DI -> far pointer to our receiver function                   */
/*                                                                  */
/*  On success AX contains the handle; CF clear.                    */
/* ================================================================ */

int pktdrv_open(WORD pktType)
{
    union  REGS  regs;
    struct SREGS segs;
    WORD         typeWord;

    if (g_pktInt < 0)
        return ERR_NOPKTDRV;

    /* Ethertype is passed in network byte order */
    typeWord = (WORD)((pktType >> 8) | (pktType << 8));

    regs.x.ax = 0x0002;
    regs.x.bx = PKT_CLASS_ETHER;
    regs.x.dx = 0xFFFF;             /* any interface number         */
    regs.x.cx = sizeof(typeWord);
    segs.ds   = FP_SEG(typeWord);
    regs.x.si = FP_OFF(typeWord);
    segs.es   = FP_SEG(receiver);
    regs.x.di = FP_OFF(receiver);

    int86x(g_pktInt, &regs, &regs, &segs);

    if (regs.x.cflag)
        return ERR_NOPKTDRV;

    g_handle  = regs.x.ax;
    g_rxReady = FALSE;
    return ERR_OK;
}

/* ================================================================ */
/*  pktdrv_close  (release_type, AX=0003h)                         */
/* ================================================================ */

void pktdrv_close(void)
{
    union REGS regs;

    if (g_pktInt < 0 || g_handle == 0)
        return;

    regs.x.ax = 0x0003;
    regs.x.bx = g_handle;
    int86(g_pktInt, &regs, &regs);

    g_handle = 0;
    g_pktInt = -1;
}

/* ================================================================ */
/*  pktdrv_send   (send_pkt, AX=0004h)                             */
/*                                                                  */
/*  DS:SI -> frame data, CX = length                               */
/* ================================================================ */

int pktdrv_send(const void far *buf, WORD len)
{
    union  REGS  regs;
    struct SREGS segs;

    if (g_pktInt < 0)
        return ERR_NOPKTDRV;

    regs.x.ax = 0x0004;
    regs.x.cx = len;
    segs.ds   = FP_SEG(buf);
    regs.x.si = FP_OFF(buf);

    int86x(g_pktInt, &regs, &regs, &segs);

    return regs.x.cflag ? ERR_IO : ERR_OK;
}

/* ================================================================ */
/*  pktdrv_get_address  (get_address, AX=0006h)                    */
/*                                                                  */
/*  Retrieve the local hardware (MAC) address.                      */
/*  BX = handle, CX = buffer size, ES:DI -> buffer                 */
/* ================================================================ */

int pktdrv_get_address(BYTE *mac)
{
    union  REGS  regs;
    struct SREGS segs;

    if (g_pktInt < 0)
        return ERR_NOPKTDRV;

    regs.x.ax = 0x0006;
    regs.x.bx = g_handle;
    regs.x.cx = ETH_ALEN;
    segs.es   = FP_SEG(mac);
    regs.x.di = FP_OFF(mac);

    int86x(g_pktInt, &regs, &regs, &segs);

    return regs.x.cflag ? ERR_IO : ERR_OK;
}

/* ================================================================ */
/*  pktdrv_get_recv_buf                                             */
/*                                                                  */
/*  Return a pointer to the most recently received frame (if any).  */
/*  Sets *len to the frame length.  Returns NULL if no frame is     */
/*  ready.  The buffer is valid until the next call.                */
/* ================================================================ */

void far *pktdrv_get_recv_buf(WORD *len)
{
    if (g_rxReady) {
        *len      = g_rxLen;
        g_rxReady = FALSE;
        return (void far *)g_rxBuf;
    }
    *len = 0;
    return NULL;
}
