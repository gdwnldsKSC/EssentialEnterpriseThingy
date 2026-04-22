/*
 * EET.C  -  EssentialEnterpriseThingy v3.2
 *
 * Main entry point: command-line argument parsing and batch execution.
 * All switches can be chained on one line or driven from a batch file.
 *
 * Compile (Borland C++ 3.1, large model):
 *   BCC -ml -O2 EET.C DRIVEMAP.C DEFRAG.C TCPIP.C PKTDRV.C
 *
 * Target: MS-DOS 5.0+, 16-bit real mode.
 */

#include "EET.H"

#define VERSION  "3.2"
#define BANNER   "EssentialEnterpriseThingy v" VERSION \
                 " -- DOS command batcher\r\n"

/* ---------------------------------------------------------------- */

static void print_usage(void)
{
    printf(BANNER);
    printf("\nUsage:\n");
    printf("  EET /MAP    <drive:> <\\\\server\\share> [password]\n");
    printf("  EET /UNMAP  <drive:>\n");
    printf("  EET /DEFRAG <drive:> [/V]\n");
    printf("  EET /PING   <a.b.c.d> [count]\n");
    printf("  EET /BATCH  <file.txt>\n");
    printf("  EET /?\n");
    printf("\nSwitches may be chained:\n");
    printf("  EET /MAP F: \\\\SRV\\TOOLS /DEFRAG C: /PING 10.0.0.1\n");
    printf("\nFor TCP/IP set: EET_IP, EET_GW, EET_MASK in environment.\n");
}

/* ---------------------------------------------------------------- */

static int do_map(int argc, char **argv, int *idx)
{
    char       driveLetter;
    const char *uncPath;
    const char *password = NULL;
    int         rc;

    if (*idx + 2 >= argc) {
        fprintf(stderr, "EET: /MAP requires drive: and \\\\server\\share\n");
        return ERR_ARGS;
    }

    driveLetter = argv[*idx + 1][0];
    uncPath     = argv[*idx + 2];
    *idx       += 2;

    /* Optional password (next token that does not start with '/') */
    if (*idx + 1 < argc && argv[*idx + 1][0] != '/') {
        password = argv[++(*idx)];
    }

    printf("Mapping %c: -> %s ...\n", driveLetter, uncPath);

    if (!redirector_present()) {
        fprintf(stderr, "EET: No DOS network redirector loaded\n");
        return ERR_NOREDIR;
    }

    rc = map_network_drive(driveLetter, uncPath, password);
    if (rc != ERR_OK) {
        fprintf(stderr, "EET: /MAP failed (DOS error %d)\n", rc);
        return rc;
    }

    printf("  Drive %c: mapped OK.\n", driveLetter);
    return ERR_OK;
}

/* ---------------------------------------------------------------- */

static int do_unmap(int argc, char **argv, int *idx)
{
    char driveLetter;
    int  rc;

    if (*idx + 1 >= argc) {
        fprintf(stderr, "EET: /UNMAP requires drive:\n");
        return ERR_ARGS;
    }

    driveLetter = argv[++(*idx)][0];
    printf("Unmapping %c: ...\n", driveLetter);

    rc = unmap_network_drive(driveLetter);
    if (rc != ERR_OK) {
        fprintf(stderr, "EET: /UNMAP failed (DOS error %d)\n", rc);
        return rc;
    }

    printf("  Drive %c: unmapped.\n", driveLetter);
    return ERR_OK;
}

/* ---------------------------------------------------------------- */

static int do_defrag(int argc, char **argv, int *idx)
{
    char driveLetter;
    BOOL verbose = FALSE;
    int  rc;

    if (*idx + 1 >= argc) {
        fprintf(stderr, "EET: /DEFRAG requires drive:\n");
        return ERR_ARGS;
    }

    driveLetter = argv[++(*idx)][0];

    if (*idx + 1 < argc && stricmp(argv[*idx + 1], "/V") == 0) {
        verbose = TRUE;
        (*idx)++;
    }

    printf("Defragmenting %c: ...\n", driveLetter);

    rc = defrag_volume(driveLetter, verbose);
    if (rc != ERR_OK) {
        fprintf(stderr, "EET: /DEFRAG failed (code %d)\n", rc);
        return rc;
    }

    printf("  Defrag of %c: complete.\n", driveLetter);
    return ERR_OK;
}

/* ---------------------------------------------------------------- */

static int do_ping(int argc, char **argv, int *idx)
{
    BYTE ip[4];
    int  count = 4;
    int  rc;

    if (*idx + 1 >= argc) {
        fprintf(stderr, "EET: /PING requires IP address\n");
        return ERR_ARGS;
    }

    if (parse_ip(argv[++(*idx)], ip) != 0) {
        fprintf(stderr, "EET: Invalid IP address '%s'\n", argv[*idx]);
        return ERR_ARGS;
    }

    if (*idx + 1 < argc && argv[*idx + 1][0] != '/') {
        count = atoi(argv[++(*idx)]);
        if (count < 1) count = 1;
    }

    printf("Pinging %d.%d.%d.%d (%d packets) ...\n",
           ip[0], ip[1], ip[2], ip[3], count);

    rc = tcpip_init();
    if (rc != ERR_OK) {
        fprintf(stderr, "EET: TCP/IP init failed (code %d)\n", rc);
        return rc;
    }

    rc = ping_host(ip, count, TRUE);
    tcpip_shutdown();

    if (rc == ERR_TIMEOUT) {
        fprintf(stderr, "EET: No reply from host.\n");
    } else if (rc != ERR_OK) {
        fprintf(stderr, "EET: /PING failed (code %d)\n", rc);
    }

    return rc;
}

/* ---------------------------------------------------------------- */

/*
 * Process a single line's worth of tokens (re-used by both the
 * real argv[] path and the /BATCH reader).
 */
static int dispatch(int argc, char **argv)
{
    int i;
    int rc = ERR_OK;

    for (i = 0; i < argc && rc == ERR_OK; i++) {
        if      (stricmp(argv[i], "/MAP")    == 0 ||
                 stricmp(argv[i], "-MAP")    == 0)
            rc = do_map   (argc, argv, &i);
        else if (stricmp(argv[i], "/UNMAP")  == 0 ||
                 stricmp(argv[i], "-UNMAP")  == 0)
            rc = do_unmap (argc, argv, &i);
        else if (stricmp(argv[i], "/DEFRAG") == 0 ||
                 stricmp(argv[i], "-DEFRAG") == 0)
            rc = do_defrag(argc, argv, &i);
        else if (stricmp(argv[i], "/PING")   == 0 ||
                 stricmp(argv[i], "-PING")   == 0)
            rc = do_ping  (argc, argv, &i);
        else if (stricmp(argv[i], "/?")      == 0 ||
                 stricmp(argv[i], "/H")      == 0 ||
                 stricmp(argv[i], "-H")      == 0) {
            print_usage();
            /* not an error — but stop further processing */
            return ERR_ARGS;
        } else {
            fprintf(stderr, "EET: Unknown option '%s'\n", argv[i]);
            print_usage();
            return ERR_ARGS;
        }
    }

    return rc;
}

/* ---------------------------------------------------------------- */

static int do_batch(int argc, char **argv, int *idx)
{
    FILE  *fp;
    char   line[256];
    char  *tokens[32];
    int    tokenCount;
    char  *tok;
    int    lineNo = 0;
    int    len;
    int    rc = ERR_OK;

    if (*idx + 1 >= argc) {
        fprintf(stderr, "EET: /BATCH requires a filename\n");
        return ERR_ARGS;
    }

    fp = fopen(argv[++(*idx)], "r");
    if (!fp) {
        fprintf(stderr, "EET: Cannot open batch file '%s'\n", argv[*idx]);
        return ERR_IO;
    }

    while (fgets(line, sizeof(line), fp)) {
        lineNo++;

        /* Strip trailing CR/LF */
        len = (int)strlen(line);
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and comment lines starting with ';' */
        if (len == 0 || line[0] == ';')
            continue;

        /* Tokenise */
        tokenCount = 0;
        tok = strtok(line, " \t");
        while (tok && tokenCount < 31) {
            tokens[tokenCount++] = tok;
            tok = strtok(NULL, " \t");
        }

        rc = dispatch(tokenCount, tokens);
        if (rc != ERR_OK) {
            fprintf(stderr, "EET: Batch error at line %d\n", lineNo);
            break;
        }
    }

    fclose(fp);
    return rc;
}

/* ================================================================ */

int main(int argc, char **argv)
{
    int i;
    int rc = ERR_OK;

    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Check for /BATCH specially so it can interleave with other ops */
    for (i = 1; i < argc && rc == ERR_OK; i++) {
        if (stricmp(argv[i], "/BATCH") == 0 ||
            stricmp(argv[i], "-BATCH") == 0) {
            rc = do_batch(argc, argv, &i);
        } else {
            /* Hand the remaining argv slice to dispatch() */
            rc = dispatch(argc - i, argv + i);
            break;   /* dispatch() consumed the rest */
        }
    }

    return (rc == ERR_OK) ? 0 : 1;
}
