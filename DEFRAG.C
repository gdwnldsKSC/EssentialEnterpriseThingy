/*
 * DEFRAG.C  -  EssentialEnterpriseThingy v3.2
 *
 * FAT12 / FAT16 disk defragmenter.
 *
 * How it works
 * ------------
 * 1. Read the boot sector (sector 0) to obtain volume geometry.
 * 2. Read the entire FAT copy 1 into a heap-allocated array.
 * 3. Walk the root directory to build a list of files and note
 *    which ones have fragmented cluster chains.
 * 4. For each fragmented file, find a contiguous run of free
 *    clusters large enough to hold the whole file.
 * 5. Copy the data sector-by-sector to the new run, update the
 *    FAT chain, and patch the directory entry start cluster.
 * 6. Write the updated FAT back to all FAT copies.
 *
 * Limitations
 * -----------
 * - Root directory only (sub-directories not recursed).
 * - FAT12 and FAT16 only; FAT32 is rejected at startup.
 * - Requires MS-DOS 3.31+ for 32-bit sector addressing.
 * - Sector I/O uses INT 25h / INT 26h (absolute read/write).
 *   These interrupts push an extra FLAGS word onto the caller's
 *   stack upon return; the inline-assembly wrappers below account
 *   for this correctly.
 *
 * References
 * ----------
 * "Microsoft MS-DOS Programmer's Reference" 6th ed., 1993.
 * "Inside the FAT File System", Microsoft KnowledgeBase Q67587.
 * Ralf Brown's Interrupt List, INT 25h / INT 26h entries.
 */

#include "EET.H"
#include <alloc.h>      /* Borland: farmalloc / farfree */

/* ---- Module-private state --------------------------------------- */

static VOLUME_INFO  g_vol;

/* File table (root-directory entries that have data) */
typedef struct {
    char  name[13];         /* "NAME.EXT\0"                       */
    WORD  startCluster;
    DWORD fileSize;
    BOOL  fragmented;
} FILE_ENTRY;

static FILE_ENTRY  g_files[MAX_FILES];
static int         g_numFiles = 0;

/* General-purpose sector buffer (enough for one max cluster) */
static BYTE  g_sectBuf[SECTOR_SIZE * 64];  /* 32 KB -- covers
                                              * largest FAT16
                                              * cluster (32 KB)  */

/* ================================================================ */
/*  Absolute sector I/O  (INT 25h / INT 26h)                       */
/*                                                                  */
/*  These two DOS interrupts are unusual: on return they leave the  */
/*  caller's original FLAGS word on the stack.  A plain int86()     */
/*  call would leave the stack unbalanced.  The safest approach on  */
/*  Borland C++ is inline assembly so we can pop the extra word     */
/*  ourselves.                                                       */
/* ================================================================ */

/*
 * read_sectors -- read <count> sectors starting at <startSec> from
 *                 <drive> (0=A, 1=B, 2=C ...) into <buf>.
 *
 * Returns ERR_OK or ERR_IO.
 */
static int read_sectors(BYTE drive, DWORD startSec, WORD count,
                        void far *buf)
{
    WORD err = 0;

    if (count == 0 || startSec > 0xFFFEL)
        return ERR_IO;

    /*
     * Borland C++ inline assembly.
     *
     * INT 25h:  AL = drive, CX = sector count, DX = start sector
     *           DS:BX -> buffer
     * On return: CF set on error (AX = error code)
     *           FLAGS pushed onto stack -- we pop them immediately.
     */
#if defined(__TURBOC__) || defined(__BORLANDC__)
    _asm {
        push ds
        mov  al,  drive
        mov  cx,  count
        mov  dx,  word ptr [startSec]   /* low 16 bits only for
                                          * FAT16 volumes <= 32 MB */
        lds  bx,  buf
        int  25h
        pop  cx                         /* discard extra FLAGS word */
        pop  ds
        jnc  ok
        mov  err, 1
    ok:
    }
#else
    /* Watcom C: use _int386 or an equivalent; supply your own glue */
    (void)drive; (void)startSec; (void)count; (void)buf;
    err = 1;   /* must replace with compiler-specific implementation */
#endif

    return err ? ERR_IO : ERR_OK;
}

/*
 * write_sectors -- write <count> sectors to <drive> starting at
 *                  <startSec> from <buf>.
 */
static int write_sectors(BYTE drive, DWORD startSec, WORD count,
                         const void far *buf)
{
    WORD err = 0;

    if (count == 0 || startSec > 0xFFFEL)
        return ERR_IO;

#if defined(__TURBOC__) || defined(__BORLANDC__)
    _asm {
        push ds
        mov  al,  drive
        mov  cx,  count
        mov  dx,  word ptr [startSec]
        lds  bx,  buf
        int  26h
        pop  cx                         /* discard extra FLAGS word */
        pop  ds
        jnc  ok
        mov  err, 1
    ok:
    }
#else
    (void)drive; (void)startSec; (void)count; (void)buf;
    err = 1;
#endif

    return err ? ERR_IO : ERR_OK;
}

/* ================================================================ */
/*  Boot sector / volume geometry                                   */
/* ================================================================ */

static int read_boot_sector(char driveLetter, VOLUME_INFO *vol)
{
    FAT_BOOT_SECTOR *bs;
    BYTE  drive = (BYTE)(toupper((unsigned char)driveLetter) - 'A');
    DWORD totalSec, dataSec;
    int   rc;

    rc = read_sectors(drive, 0L, 1, g_sectBuf);
    if (rc != ERR_OK)
        return rc;

    bs = (FAT_BOOT_SECTOR *)g_sectBuf;

    if (bs->bytesPerSec != SECTOR_SIZE) {
        fprintf(stderr, "EET: Unsupported sector size %u\n",
                bs->bytesPerSec);
        return ERR_NOFAT;
    }

    vol->driveNum    = drive;
    vol->bytesPerSec = bs->bytesPerSec;
    vol->secPerClust = bs->secPerClust;
    vol->rsvdSecCnt  = bs->rsvdSecCnt;
    vol->numFATs     = bs->numFATs;
    vol->rootEntCnt  = bs->rootEntCnt;
    vol->fatSz16     = bs->fatSz16;

    /* LBA of first root-directory sector */
    vol->rootDirSec  = (DWORD)vol->rsvdSecCnt
                     + (DWORD)vol->numFATs * (DWORD)vol->fatSz16;

    /* LBA of first data sector (cluster 2) */
    vol->firstDataSec = vol->rootDirSec
                      + (((DWORD)vol->rootEntCnt * 32UL)
                         + (vol->bytesPerSec - 1))
                        / vol->bytesPerSec;

    totalSec = (bs->totSec16 != 0)
               ? (DWORD)bs->totSec16
               : bs->totSec32;
    dataSec  = totalSec - vol->firstDataSec;
    vol->totalClusters = (WORD)(dataSec / vol->secPerClust);

    if (vol->totalClusters > 65524U) {
        fprintf(stderr, "EET: FAT32 volumes are not supported\n");
        return ERR_NOFAT;
    }

    return ERR_OK;
}

/* ================================================================ */
/*  FAT I/O                                                         */
/* ================================================================ */

/*
 * Read the first FAT copy from disk into a heap-allocated WORD
 * array indexed directly by cluster number (vol->fat[clusterNum]).
 * Handles both FAT12 and FAT16 packing.
 */
static int read_fat(VOLUME_INFO *vol)
{
    DWORD    fatBytes = (DWORD)vol->fatSz16 * SECTOR_SIZE;
    BYTE far *raw;
    WORD     i;
    int      rc;

    vol->fat = (WORD *)farmalloc((DWORD)(vol->totalClusters + 2)
                                 * sizeof(WORD));
    if (!vol->fat) {
        fprintf(stderr, "EET: Out of memory (FAT array)\n");
        return ERR_NOMEM;
    }

    raw = (BYTE far *)farmalloc(fatBytes);
    if (!raw) {
        farfree(vol->fat);
        vol->fat = NULL;
        return ERR_NOMEM;
    }

    rc = read_sectors(vol->driveNum, (DWORD)vol->rsvdSecCnt,
                      vol->fatSz16, raw);
    if (rc != ERR_OK) {
        farfree(raw);
        farfree(vol->fat);
        vol->fat = NULL;
        return rc;
    }

    if (vol->totalClusters >= 4085) {
        /* FAT16: every entry is exactly 2 bytes, little-endian */
        for (i = 0; i <= vol->totalClusters + 1; i++) {
            BYTE far *p   = raw + (DWORD)i * 2;
            vol->fat[i]   = (WORD)(*p) | ((WORD)(*(p + 1)) << 8);
        }
    } else {
        /* FAT12: entries are packed 3 bytes per 2 clusters */
        for (i = 0; i <= vol->totalClusters + 1; i++) {
            DWORD    byteOff = ((DWORD)i * 3UL) / 2UL;
            BYTE far *lo     = raw + byteOff;
            BYTE far *hi     = raw + byteOff + 1;
            if (i & 1)
                vol->fat[i] = ((WORD)(*lo) >> 4)
                             | ((WORD)(*hi) << 4);
            else
                vol->fat[i] = (WORD)(*lo)
                             | (((WORD)(*hi) & 0x0F) << 8);
        }
    }

    farfree(raw);
    return ERR_OK;
}

/*
 * Pack the in-memory FAT back to disk, writing all FAT copies.
 */
static int write_fat(VOLUME_INFO *vol)
{
    DWORD    fatBytes = (DWORD)vol->fatSz16 * SECTOR_SIZE;
    BYTE far *raw;
    WORD     i;
    BYTE     f;
    int      rc;

    raw = (BYTE far *)farmalloc(fatBytes);
    if (!raw)
        return ERR_NOMEM;

    _fmemset(raw, 0, fatBytes);

    if (vol->totalClusters >= 4085) {
        /* FAT16 */
        for (i = 0; i <= vol->totalClusters + 1; i++) {
            BYTE far *p = raw + (DWORD)i * 2;
            *p       = (BYTE)(vol->fat[i] & 0xFF);
            *(p + 1) = (BYTE)(vol->fat[i] >> 8);
        }
    } else {
        /* FAT12 */
        for (i = 0; i <= vol->totalClusters + 1; i++) {
            DWORD    byteOff = ((DWORD)i * 3UL) / 2UL;
            BYTE far *lo     = raw + byteOff;
            BYTE far *hi     = raw + byteOff + 1;
            if (i & 1) {
                *lo = (*lo & 0x0F)
                    | (BYTE)((vol->fat[i] & 0x0F) << 4);
                *hi = (BYTE)(vol->fat[i] >> 4);
            } else {
                *lo = (BYTE)(vol->fat[i] & 0xFF);
                *hi = (*hi & 0xF0)
                    | (BYTE)((vol->fat[i] >> 8) & 0x0F);
            }
        }
    }

    for (f = 0; f < vol->numFATs; f++) {
        DWORD startSec = (DWORD)vol->rsvdSecCnt
                       + (DWORD)f * vol->fatSz16;
        rc = write_sectors(vol->driveNum, startSec,
                           vol->fatSz16, raw);
        if (rc != ERR_OK) {
            farfree(raw);
            return rc;
        }
    }

    farfree(raw);
    return ERR_OK;
}

/* ================================================================ */
/*  Cluster utilities                                               */
/* ================================================================ */

/* Return the first LBA of a given cluster number */
static DWORD cluster_to_lba(VOLUME_INFO *vol, WORD cluster)
{
    return vol->firstDataSec
         + (DWORD)(cluster - 2) * (DWORD)vol->secPerClust;
}

/* Count how many clusters are in a chain starting at startCluster */
static WORD chain_length(VOLUME_INFO *vol, WORD startCluster)
{
    WORD count = 0;
    WORD cur   = startCluster;

    while (cur >= 2 && cur < FAT_EOC && count <= MAX_CLUSTERS) {
        cur = vol->fat[cur];
        count++;
    }
    return count;
}

/* Return TRUE if the chain is already contiguous on disk */
static BOOL is_contiguous(VOLUME_INFO *vol, WORD startCluster)
{
    WORD cur  = startCluster;
    WORD prev = 0;

    while (cur >= 2 && cur < FAT_EOC) {
        if (prev != 0 && cur != (WORD)(prev + 1))
            return FALSE;
        prev = cur;
        cur  = vol->fat[cur];
    }
    return TRUE;
}

/*
 * Find the first contiguous run of <needed> free clusters on the
 * volume, starting the search at cluster <hint>.  The search wraps
 * around if no suitable run is found before the end of the FAT.
 * Returns the first cluster of the run, or 0 if none was found.
 */
static WORD find_free_run(VOLUME_INFO *vol, WORD needed, WORD hint)
{
    WORD start = 0;
    WORD count = 0;
    WORD i;
    WORD end   = vol->totalClusters + 1;

    /* Forward pass from hint */
    for (i = hint; i <= end; i++) {
        if (vol->fat[i] == FAT_FREE) {
            if (count == 0) start = i;
            if (++count >= needed) return start;
        } else {
            count = 0;
        }
    }

    /* Wrap-around from cluster 2 up to hint */
    count = 0;
    for (i = 2; i < hint; i++) {
        if (vol->fat[i] == FAT_FREE) {
            if (count == 0) start = i;
            if (++count >= needed) return start;
        } else {
            count = 0;
        }
    }

    return 0;   /* not found */
}

/* ================================================================ */
/*  File-level defrag                                               */
/* ================================================================ */

/*
 * Copy every cluster of a fragmented file to a fresh contiguous
 * run, update the FAT chain, and record the new start cluster in
 * fe->startCluster.  The caller must still update the directory
 * entry on disk.
 */
static int defrag_file(VOLUME_INFO *vol, FILE_ENTRY *fe, BOOL verbose)
{
    WORD numClusters = chain_length(vol, fe->startCluster);
    WORD newStart;
    WORD cur, next;
    WORD i;
    int  rc;

    if (numClusters == 0)
        return ERR_OK;

    /* Need a g_sectBuf big enough for one cluster */
    if ((WORD)vol->secPerClust * SECTOR_SIZE > sizeof(g_sectBuf)) {
        fprintf(stderr, "EET: Cluster too large for sector buffer\n");
        return ERR_IO;
    }

    newStart = find_free_run(vol, numClusters, 2);
    if (newStart == 0) {
        fprintf(stderr,
                "EET: Not enough contiguous space for %s\n",
                fe->name);
        return ERR_IO;
    }

    if (verbose)
        printf("    %-12s  %4u clusters  %u -> %u\n",
               fe->name, numClusters,
               fe->startCluster, newStart);

    /* --- Copy data and free old clusters ----------------------- */
    cur = fe->startCluster;
    for (i = 0; i < numClusters && cur >= 2 && cur < FAT_EOC; i++) {
        DWORD srcLBA = cluster_to_lba(vol, cur);
        DWORD dstLBA = cluster_to_lba(vol, newStart + i);

        rc = read_sectors(vol->driveNum, srcLBA,
                          vol->secPerClust, g_sectBuf);
        if (rc != ERR_OK) return rc;

        rc = write_sectors(vol->driveNum, dstLBA,
                           vol->secPerClust, g_sectBuf);
        if (rc != ERR_OK) return rc;

        next = vol->fat[cur];
        vol->fat[cur] = FAT_FREE;   /* release old cluster */
        cur = next;
    }

    /* --- Rebuild contiguous FAT chain -------------------------- */
    for (i = 0; i < (WORD)(numClusters - 1); i++)
        vol->fat[newStart + i] = newStart + i + 1;
    vol->fat[newStart + numClusters - 1] = 0xFFFF;  /* EOC */

    fe->startCluster = newStart;
    return ERR_OK;
}

/* ================================================================ */
/*  Directory scanner                                               */
/* ================================================================ */

static int scan_root_dir(VOLUME_INFO *vol)
{
    FAT_DIR_ENTRY *de;
    DWORD lba          = vol->rootDirSec;
    WORD  remaining    = vol->rootEntCnt;
    WORD  perSector    = (WORD)(SECTOR_SIZE / sizeof(FAT_DIR_ENTRY));
    WORD  i;
    int   rc;

    while (remaining > 0 && g_numFiles < MAX_FILES) {
        WORD toRead = (remaining > perSector) ? perSector : remaining;

        rc = read_sectors(vol->driveNum, lba, 1, g_sectBuf);
        if (rc != ERR_OK) return rc;

        de = (FAT_DIR_ENTRY *)g_sectBuf;

        for (i = 0; i < toRead && g_numFiles < MAX_FILES; i++, de++) {
            FILE_ENTRY *fe;
            int  j, k;

            if ((BYTE)de->name[0] == 0x00) goto done; /* end       */
            if ((BYTE)de->name[0] == 0xE5) continue;  /* deleted   */
            if (de->attr & 0x08)            continue;  /* vol label */
            if (de->attr & 0x10)            continue;  /* subdir    */
            if (de->fstClus < 2)            continue;  /* no data   */

            fe = &g_files[g_numFiles++];

            /* Build printable "NAME.EXT" */
            k = 0;
            for (j = 0; j < 8 && de->name[j] != ' '; j++)
                fe->name[k++] = de->name[j];
            if (de->ext[0] != ' ') {
                fe->name[k++] = '.';
                for (j = 0; j < 3 && de->ext[j] != ' '; j++)
                    fe->name[k++] = de->ext[j];
            }
            fe->name[k] = '\0';

            fe->startCluster = de->fstClus;
            fe->fileSize     = de->fileSize;
            fe->fragmented   = !is_contiguous(vol, de->fstClus);
        }

        remaining -= toRead;
        lba++;
    }
done:
    return ERR_OK;
}

/* ================================================================ */
/*  Directory-entry update (patch start cluster after move)        */
/* ================================================================ */

static int update_dir_entry(VOLUME_INFO *vol,
                             const char *name, WORD newCluster)
{
    FAT_DIR_ENTRY *de;
    DWORD lba       = vol->rootDirSec;
    WORD  remaining = vol->rootEntCnt;
    WORD  perSector = (WORD)(SECTOR_SIZE / sizeof(FAT_DIR_ENTRY));
    WORD  i;
    int   rc;

    while (remaining > 0) {
        WORD toRead = (remaining > perSector) ? perSector : remaining;

        rc = read_sectors(vol->driveNum, lba, 1, g_sectBuf);
        if (rc != ERR_OK) return rc;

        de = (FAT_DIR_ENTRY *)g_sectBuf;

        for (i = 0; i < toRead; i++, de++) {
            char ename[13];
            int  j, k = 0;

            if ((BYTE)de->name[0] == 0x00) return ERR_OK;
            if ((BYTE)de->name[0] == 0xE5) continue;
            if (de->attr & 0x18)            continue;

            for (j = 0; j < 8 && de->name[j] != ' '; j++)
                ename[k++] = de->name[j];
            if (de->ext[0] != ' ') {
                ename[k++] = '.';
                for (j = 0; j < 3 && de->ext[j] != ' '; j++)
                    ename[k++] = de->ext[j];
            }
            ename[k] = '\0';

            if (stricmp(ename, name) == 0) {
                de->fstClus = newCluster;
                return write_sectors(vol->driveNum, lba, 1,
                                     g_sectBuf);
            }
        }

        remaining -= toRead;
        lba++;
    }

    return ERR_OK;   /* file not found is non-fatal */
}

/* ================================================================ */
/*  Public entry point                                              */
/* ================================================================ */

int defrag_volume(char driveLetter, BOOL verbose)
{
    int  i;
    int  fragCount  = 0;
    int  fixedCount = 0;
    int  rc;

    memset(&g_vol, 0, sizeof(g_vol));
    g_numFiles = 0;

    rc = read_boot_sector(driveLetter, &g_vol);
    if (rc != ERR_OK) return rc;

    rc = read_fat(&g_vol);
    if (rc != ERR_OK) return rc;

    rc = scan_root_dir(&g_vol);
    if (rc != ERR_OK) {
        farfree(g_vol.fat);
        return rc;
    }

    for (i = 0; i < g_numFiles; i++) {
        if (g_files[i].fragmented)
            fragCount++;
    }

    printf("  %d file(s) scanned, %d fragmented.\n",
           g_numFiles, fragCount);

    if (fragCount == 0) {
        if (verbose)
            printf("  Volume is already optimized.\n");
        farfree(g_vol.fat);
        return ERR_OK;
    }

    if (verbose)
        printf("  Moving fragmented files:\n");

    for (i = 0; i < g_numFiles && rc == ERR_OK; i++) {
        if (!g_files[i].fragmented)
            continue;

        rc = defrag_file(&g_vol, &g_files[i], verbose);
        if (rc == ERR_OK) {
            rc = update_dir_entry(&g_vol, g_files[i].name,
                                  g_files[i].startCluster);
            if (rc == ERR_OK)
                fixedCount++;
        }
    }

    if (rc == ERR_OK)
        rc = write_fat(&g_vol);

    farfree(g_vol.fat);

    if (rc == ERR_OK)
        printf("  Fixed %d / %d fragmented file(s).\n",
               fixedCount, fragCount);

    return rc;
}
