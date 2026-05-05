/*
 * wol.c
 *
 * Wake-On-LAN sender for Windows, Linux, and macOS.
 * A single-file console application written in C (C17).
 *
 * Copyright 2026 Jens Bråkenhielm
 * SPDX-License-Identifier: MIT
 *
 * Build with:
 *   On Windows (needs MSVC toolchain initialized):
 *      release: cl.exe /nologo /W4 /WX /O2 /std:c17 /MT /DNDEBUG /Fe:wol.exe wol.c /link /SUBSYSTEM:CONSOLE /MACHINE:X64
 *      debug:   cl.exe /nologo /W4 /WX /Od /Zi /std:c17 /MTd /DDEBUG /DWOL_SELF_TEST /Fe:wold.exe wol.c /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /DEBUG
 *
 *   On Linux:
 *      release: cc -Wall -Wextra -Werror -O2 -std=c17 -static -DNDEBUG -o wol wol.c
 *      debug:   cc -Wall -Wextra -Werror -g -O0 -std=c17 -DDEBUG -DWOL_SELF_TEST -o wold wol.c
 *
 *   On macOS:
 *      release: cc -Wall -Wextra -Werror -O2 -std=c17 -DNDEBUG -o wol wol.c
 *      debug:   cc -Wall -Wextra -Werror -g -O0 -std=c17 -DDEBUG -DWOL_SELF_TEST -o wold wol.c
 *
 *   Or use the build scripts bundled with this file:
 *      build.bat (Windows) — initialises the MSVC toolchain automatically if needed
 *      build.sh  (Linux, macOS)
 *
 * Program flow:
 *   (debug builds: --self-test runs the built-in test suite and exits)
 *   -> parse and validate CLI options (broadcast accepts plain IPv4 or CIDR notation)
 *   -> collect and validate MAC addresses (from file and/or CLI args)
 *   -> initialise network
 *   -> open UDP socket
 *   -> send magic packet for each MAC and report result
 *   -> exit 0 if all packets sent successfully, 1 if any failed
 *
 * All functions are file-scoped (static). main() is the only external symbol.
 * Resources acquired in main are released through a single cleanup label (cleanup:),
 * the idiomatic C pattern for error paths that share teardown logic.
 */

/* ── Platform compatibility ──────────────────────────────────────────────── */
/*
 * Platform-specific includes, type definitions, and function-like macros
 * (net_close, net_error, net_cleanup, str_icmp) are consolidated here.
 * Where Windows and POSIX differ in behaviour rather than naming, the
 * relevant function carries its own #ifdef _WIN32 block.
 */
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define _CRT_SECURE_NO_WARNINGS  /* suppress MSVC deprecation of standard fopen/etc. */

    #include <winsock2.h>    /* SOCKET, socket(), sendto(), etc. */
    #include <ws2tcpip.h>    /* inet_pton(), socklen_t */

    #pragma comment(lib, "ws2_32.lib")

    #define net_close(s)     closesocket(s)
    #define net_error()      WSAGetLastError()
    #define net_cleanup()    WSACleanup()
    #define str_icmp(a, b)   _stricmp((a), (b))
#else
    #define _POSIX_C_SOURCE  200809L  /* expose strcasecmp() in <strings.h> */

    #include <sys/socket.h>  /* socket(), sendto(), setsockopt() */
    #include <netinet/in.h>  /* struct sockaddr_in, htons() */
    #include <arpa/inet.h>   /* inet_pton() */
    #include <unistd.h>      /* close() */
    #include <errno.h>       /* errno */
    #include <strings.h>     /* strcasecmp() */

    typedef int SOCKET;

    #define INVALID_SOCKET   (-1)
    #define SOCKET_ERROR     (-1)
    #define BOOL             int
    #define TRUE             1

    #define net_close(s)     close(s)
    #define net_error()      errno
    #define net_cleanup()    ((void)0)
    #define str_icmp(a, b)   strcasecmp((a), (b))
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WOL_VERSION         "1.0.4"
#define DEFAULT_BROADCAST   "255.255.255.255"
#define DEFAULT_PORT        9
#define MAC_LEN             6
#define MAC_REPEATS         16  /* WoL spec: target MAC repeated 16 times after the sync header */
#define PACKET_HEADER_LEN   6   /* six 0xFF bytes that signal WoL to the NIC */
#define PACKET_LEN          (PACKET_HEADER_LEN + MAC_REPEATS * MAC_LEN)  /* 102 bytes total */

typedef unsigned char byte;

enum action { ACT_RUN, ACT_HELP, ACT_VERSION };

struct cli {
    enum action    action;
    struct in_addr broadcast_addr;  /* validated IPv4 from -b/--broadcast */
    const char    *mac_file_path;   /* path from -f/--file, or NULL; I/O validation deferred */
    unsigned short port;            /* validated UDP port from -p/--port */
    int            first_mac_index; /* argv index of the first positional (MAC) argument */
};

struct mac {
    byte value[MAC_LEN];
    char text[18];   /* "AA:BB:CC:DD:EE:FF\0" */
};


/* ── MAC address parsing and formatting ──────────────────────────────────── */

/* Returns the numeric value (0-15) of a hex digit (case-insensitive), or 16 if invalid. */
static unsigned hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;  /* fold to lowercase (ASCII bit 5) */
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10U : 16U;
}

/* Parses a MAC address in any supported format into out[MAC_LEN]. Returns 1 on
 * success, 0 if text is NULL or not a valid MAC.
 * Accepted: AABBCCDDEEFF  AA:BB:CC:DD:EE:FF  AA-BB-CC-DD-EE-FF
 *           AA.BB.CC.DD.EE.FF  AABB.CCDD.EEFF */
static int parse_mac(const char *text, byte out[MAC_LEN])
{
    const char *p;
    size_t len;
    int i;
    char sep;

    if (!text) return 0;
    len = strlen(text);

    if (len == 12) {
        sep = 0;
    } else if (len == 17) {
        sep = text[2];
        if (sep != ':' && sep != '-' && sep != '.') return 0;
        if (text[5]!=sep || text[8]!=sep || text[11]!=sep || text[14]!=sep) return 0;
    } else if (len == 14) {
        sep = '.';
        if (text[4] != '.' || text[9] != '.') return 0;
    } else {
        return 0;
    }

    for (i = 0, p = text; i < MAC_LEN; ++i) {
        unsigned hi = hex_nibble((unsigned char)p[0]);
        unsigned lo = hex_nibble((unsigned char)p[1]);
        if ((hi | lo) > 15) return 0;
        out[i] = (byte)((hi << 4) | lo);
        p += 2;
        if (sep && *p == sep) ++p;
    }
    return 1;
}

/* Writes the MAC as uppercase "AA:BB:CC:DD:EE:FF\0" into out (at least 18 bytes). */
static void format_mac(const byte mac[MAC_LEN], char out[18])
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    int pos = 0;

    for (i = 0; i < MAC_LEN; ++i) {
        out[pos++] = hex[(mac[i] >> 4) & 0x0F];
        out[pos++] = hex[mac[i] & 0x0F];
        if (i + 1 < MAC_LEN) {
            out[pos++] = ':';
        }
    }
    out[pos] = '\0';
}


/* ── MAC file input ──────────────────────────────────────────────────────── */
/*
 * Two passes over the file: scan_mac_file validates and counts (enabling a
 * single exact allocation), load_mac_file fills. Avoids realloc complexity.
 * next_mac_line owns the file format definition shared by both passes.
 */

/* Reads the next meaningful line from a MAC file into buf, trimming leading and
 * trailing whitespace. Blank lines and lines beginning with '#' are skipped.
 * Increments *lineno for every line read; pass NULL if tracking is not needed.
 * Returns 1 if a line was found, 0 at EOF. */
static int next_mac_line(FILE *f, char *buf, int bufsize, int *lineno)
{
    while (fgets(buf, bufsize, f) != NULL) {
        char *p = buf;
        char *end;

        if (lineno) ++(*lineno);

        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;

        end = p + strlen(p) - 1;
        while (end > p && (unsigned char)*end <= ' ') *end-- = '\0';

        if (p > buf) memmove(buf, p, strlen(p) + 1);
        return 1;
    }
    return 0;
}

/* First pass: validates every meaningful line as a MAC address and returns the
 * count. On the first invalid entry, prints an error with the line number and
 * returns -1. The file is left at EOF; the caller must rewind before load_mac_file. */
static int scan_mac_file(FILE *f, const char *path)
{
    char line[256];
    byte dummy[MAC_LEN];
    int lineno = 0, count = 0;

    while (next_mac_line(f, line, sizeof(line), &lineno)) {
        if (!parse_mac(line, dummy)) {
            fprintf(stderr, "Error: invalid MAC address on line %d of %s: %s\n",
                    lineno, path, line);
            return -1;
        }
        ++count;
    }
    return count;
}

/* Second pass: loads MAC entries from an already-open, rewound file into
 * macs[] starting at macs[offset]. All entries are valid (guaranteed by
 * scan_mac_file); this pass only parses and formats. */
static void load_mac_file(FILE *f, struct mac *macs, int offset)
{
    char line[256];
    int index = offset;

    while (next_mac_line(f, line, sizeof(line), NULL)) {
        parse_mac(line, macs[index].value);
        format_mac(macs[index].value, macs[index].text);
        ++index;
    }
}


/* ── Network ─────────────────────────────────────────────────────────────── */

/* Fills buf with the system description of err, always NUL-terminating.
 * On Windows, FormatMessage appends \r\n — this function strips that trailing
 * whitespace so the string can be embedded directly in fprintf format strings. */
static void net_error_str(int err, char *buf, size_t buf_len)
{
#ifdef _WIN32
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                              NULL, (DWORD)err, 0, buf, (DWORD)(buf_len - 1), NULL);
    while (n > 0 && (unsigned char)buf[n - 1] <= ' ')
        buf[--n] = '\0';
    if (n == 0)
        snprintf(buf, buf_len, "unknown error");
#else
    if (strerror_r(err, buf, buf_len) != 0)
        snprintf(buf, buf_len, "unknown error");
#endif
}

/* Parses a decimal string as a UDP port number (1-65535). Returns 1 on success. */
static int parse_port(const char *text, unsigned short *port_out)
{
    char *end;
    unsigned long value;

    if (text == NULL || *text == '\0') {
        return 0;
    }

    value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0UL || value > 65535UL) {
        return 0;
    }

    *port_out = (unsigned short)value;
    return 1;
}

/* Parses a broadcast address from a plain IPv4 string or CIDR notation
 * (e.g. 192.168.1.50/24). CIDR input yields the directed broadcast address
 * for that subnet. Returns 1 on success, 0 on invalid input. */
static int parse_broadcast(const char *text, struct in_addr *addr_out)
{
    const char *slash;
    char ip_buf[16];
    size_t ip_len;
    char *end;
    unsigned long prefix;
    struct in_addr addr;
    uint32_t ip, mask, bcast;

    slash = strchr(text, '/');
    if (slash == NULL)
        return inet_pton(AF_INET, text, addr_out) == 1;

    ip_len = (size_t)(slash - text);
    if (ip_len == 0 || ip_len >= sizeof(ip_buf)) return 0;
    memcpy(ip_buf, text, ip_len);
    ip_buf[ip_len] = '\0';

    if (inet_pton(AF_INET, ip_buf, &addr) != 1) return 0;

    if (slash[1] == '\0') return 0;
    prefix = strtoul(slash + 1, &end, 10);
    if (*end != '\0' || prefix > 32) return 0;

    /* Arithmetic requires host byte order (inet_pton stores network/big-endian).
     * prefix=0 handled separately: shifting a uint32_t by 32 is undefined behaviour. */
    ip    = (uint32_t)ntohl(addr.s_addr);
    mask  = (prefix == 0u) ? 0u : ~(uint32_t)0u << (32u - (uint32_t)prefix);
    bcast = ip | ~mask;          /* directed broadcast: set all host bits */
    addr.s_addr = htonl(bcast);

    *addr_out = addr;
    return 1;
}

/* Assembles a sockaddr_in from an already-validated address and port. Cannot fail. */
static void build_dest_addr(struct in_addr addr, unsigned short port, struct sockaddr_in *dest)
{
    memset(dest, 0, sizeof(*dest));
    dest->sin_family = AF_INET;
    dest->sin_port   = htons(port);
    dest->sin_addr   = addr;
}

/* Initialises the network subsystem. On Windows, loads Winsock 2.2; on Linux
 * and macOS this is trivial (POSIX sockets need no initialisation). Returns 1 on success. */
static int initialize_network(void)
{
#ifdef _WIN32
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);

    if (rc != 0) {
        char errbuf[256];
        net_error_str(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "Error: WSAStartup failed: %s (%d)\n", errbuf, rc);
        return 0;
    }

    if (LOBYTE(wsa.wVersion) != 2 || HIBYTE(wsa.wVersion) != 2) {
        fprintf(stderr, "Error: Winsock 2.2 not available\n");
        net_cleanup();
        return 0;
    }
#endif
    return 1;
}

/* Opens a UDP socket with SO_BROADCAST enabled.
 * Stores the socket handle in *sock_out. Caller must net_close() on success. */
static int open_udp_socket(SOCKET *sock_out)
{
    SOCKET sock;
    BOOL on = TRUE;
    int rc;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        int err = net_error();
        char errbuf[256];
        net_error_str(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "Error: socket failed: %s (%d)\n", errbuf, err);
        return 0;
    }

    rc = setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                    (const char *)&on, (socklen_t)sizeof(on));
    if (rc == SOCKET_ERROR) {
        int err = net_error();
        char errbuf[256];
        net_error_str(err, errbuf, sizeof(errbuf));
        net_close(sock);
        fprintf(stderr, "Error: setsockopt(SO_BROADCAST) failed: %s (%d)\n", errbuf, err);
        return 0;
    }

    *sock_out = sock;
    return 1;
}

/* Builds and transmits the WoL magic packet for the given MAC address.
 * Returns 1 on success. On failure, *error_out holds the socket error
 * number, or 0 if sendto delivered fewer bytes than expected. */
static int send_magic_packet(SOCKET sock, const struct sockaddr_in *dest,
                             const byte mac[MAC_LEN], int *error_out)
{
    byte packet[PACKET_LEN];
    int i, sent;

    memset(packet, 0xFF, PACKET_HEADER_LEN);
    for (i = 0; i < MAC_REPEATS; ++i)
        memcpy(packet + PACKET_HEADER_LEN + i * MAC_LEN, mac, MAC_LEN);

    sent = (int)sendto(sock, (const char *)packet, PACKET_LEN, 0,
                       (const struct sockaddr *)dest, (socklen_t)sizeof(*dest));
    if (sent == SOCKET_ERROR) { *error_out = net_error(); return 0; }
    *error_out = 0;
    return sent == PACKET_LEN;
}


/* ── CLI argument handling ───────────────────────────────────────────────── */

/* Returns 1 if arg is any recognised help flag: -h or --help (all platforms),
 * or /? (Windows only; guarded to avoid shell glob expansion on Linux). */
static int is_help_arg(const char *arg)
{
    if (str_icmp(arg, "-h") == 0 || str_icmp(arg, "--help") == 0) return 1;
#ifdef _WIN32
    if (strcmp(arg, "/?") == 0) return 1;
#endif
    return 0;
}

/* Prints usage information and accepted MAC formats to stdout. */
static void print_help(const char *prog)
{
    printf("Usage: %s [options] <MAC> [<MAC> ...]\n\n", prog);
    printf("Options:\n");
    printf("  -b, --broadcast <addr>   Broadcast address: IPv4 or IPv4/prefix (default: %s)\n", DEFAULT_BROADCAST);
    printf("  -p, --port <port>        UDP port number (default: %d)\n", DEFAULT_PORT);
    printf("  -f, --file <path>        Read MAC addresses from a file (one per line)\n");
    printf("      --version            Show version number and exit\n");
#ifdef _WIN32
    printf("  -h, --help, /?           Show this help text\n\n");
#else
    printf("  -h, --help               Show this help text\n\n");
#endif
    printf("Accepted MAC formats:\n");
    printf("  AA:BB:CC:DD:EE:FF\n");
    printf("  AA-BB-CC-DD-EE-FF\n");
    printf("  AA.BB.CC.DD.EE.FF\n");
    printf("  AABBCCDDEEFF\n");
    printf("  AABB.CCDD.EEFF\n\n");
    printf("MAC file format:\n");
    printf("  One MAC address per line. Blank lines and lines where '#' is the first\n");
    printf("  non-whitespace character are ignored.\n\n");
    printf("Examples:\n");
    printf("  %s 00:11:22:33:44:55 66-77-88-99-AA-BB\n", prog);
    printf("  %s -f macs.txt -b 192.168.10.255\n", prog);
    printf("  %s -b 192.168.10.255 -p 9 AABBCCDDEEFF\n", prog);
    printf("  %s -b 192.168.10.50/24 AABBCCDDEEFF\n", prog);
}

/* Parses and validates command-line arguments into opts. Returns 1 on success
 * (including when a help or version action is found), or 0 if an unrecognised
 * option, missing value, or invalid option value is seen. Options must appear
 * before MAC addresses; "--" ends option parsing. */
static int parse_cli(int argc, char *argv[], struct cli *opts)
{
    int i;

    opts->action          = ACT_RUN;
    opts->mac_file_path   = NULL;
    opts->port            = DEFAULT_PORT;
    opts->first_mac_index = 1;
    inet_pton(AF_INET, DEFAULT_BROADCAST, &opts->broadcast_addr);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            opts->first_mac_index = i + 1;
            return 1;
        }

        if (is_help_arg(argv[i])) {
            opts->action = ACT_HELP;
            return 1;
        }

        if (str_icmp(argv[i], "--version") == 0) {
            opts->action = ACT_VERSION;
            return 1;
        }

        if (str_icmp(argv[i], "-f") == 0 || str_icmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing value after %s\n", argv[i]);
                return 0;
            }
            opts->mac_file_path = argv[++i];
            continue;
        }

        if (str_icmp(argv[i], "-b") == 0 || str_icmp(argv[i], "--broadcast") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing value after %s\n", argv[i]);
                return 0;
            }
            if (!parse_broadcast(argv[++i], &opts->broadcast_addr)) {
                fprintf(stderr, "Error: invalid broadcast address: %s\n", argv[i]);
                return 0;
            }
            continue;
        }

        if (str_icmp(argv[i], "-p") == 0 || str_icmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing value after %s\n", argv[i]);
                return 0;
            }
            if (!parse_port(argv[++i], &opts->port)) {
                fprintf(stderr, "Error: invalid UDP port: %s\n", argv[i]);
                return 0;
            }
            continue;
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            return 0;
        }

        /* First non-option argument: the MAC list starts here. */
        opts->first_mac_index = i;
        return 1;
    }

    opts->first_mac_index = argc;
    return 1;
}


/* ── Self-test suite ─────────────────────────────────────────────────────── */
/*
 * Compiled only when WOL_SELF_TEST is defined (debug builds). Activated by
 * --self-test; not listed in --help. Exits 0 on pass, 1 on any failure.
 */
#ifdef WOL_SELF_TEST

static int tests_run, tests_failed;

#define CHECK(cond) do { \
    ++tests_run; \
    if (!(cond)) { \
        ++tests_failed; \
        fprintf(stderr, "FAIL  line %-4d  %s\n", __LINE__, #cond); \
    } \
} while (0)

static void test_parse_mac(void)
{
    byte mac[MAC_LEN];

    /* Valid: all five formats */
    CHECK(parse_mac("AABBCCDDEEFF", mac));
    CHECK(mac[0] == 0xAA && mac[3] == 0xDD && mac[5] == 0xFF);

    CHECK(parse_mac("AA:BB:CC:DD:EE:FF", mac));
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF);

    CHECK(parse_mac("AA-BB-CC-DD-EE-FF", mac));
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF);

    CHECK(parse_mac("AA.BB.CC.DD.EE.FF", mac));
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF);

    CHECK(parse_mac("AABB.CCDD.EEFF", mac));
    CHECK(mac[0] == 0xAA && mac[2] == 0xCC && mac[5] == 0xFF);

    /* Lowercase and mixed case */
    CHECK(parse_mac("aa:bb:cc:dd:ee:ff", mac));
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF);

    CHECK(parse_mac("aAbBcCdDeEfF", mac));
    CHECK(mac[0] == 0xAA && mac[5] == 0xFF);

    /* Invalid: NULL, bad length, bad hex, mixed separators, spaces */
    CHECK(!parse_mac(NULL, mac));
    CHECK(!parse_mac("", mac));
    CHECK(!parse_mac("AABBCCDDEE", mac));
    CHECK(!parse_mac("AABBCCDDEEFF00", mac));
    CHECK(!parse_mac("GG:BB:CC:DD:EE:FF", mac));
    CHECK(!parse_mac("AA:BB-CC:DD:EE:FF", mac));
    CHECK(!parse_mac("AA BB CC DD EE FF", mac));
}

static void test_parse_port(void)
{
    unsigned short port;

    /* Valid */
    CHECK(parse_port("1", &port) && port == 1);
    CHECK(parse_port("9", &port) && port == 9);
    CHECK(parse_port("65535", &port) && port == 65535);

    /* Invalid */
    CHECK(!parse_port("0", &port));
    CHECK(!parse_port("65536", &port));
    CHECK(!parse_port("", &port));
    CHECK(!parse_port("abc", &port));
    CHECK(!parse_port(NULL, &port));
    CHECK(!parse_port("9x", &port));
}

static void test_parse_broadcast(void)
{
    struct in_addr addr, expected;

    /* Plain IPv4 */
    CHECK(parse_broadcast("255.255.255.255", &addr));
    inet_pton(AF_INET, "255.255.255.255", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    CHECK(parse_broadcast("192.168.1.255", &addr));
    inet_pton(AF_INET, "192.168.1.255", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    /* CIDR: host address yields directed broadcast */
    CHECK(parse_broadcast("192.168.1.50/24", &addr));
    inet_pton(AF_INET, "192.168.1.255", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    CHECK(parse_broadcast("10.0.0.1/8", &addr));
    inet_pton(AF_INET, "10.255.255.255", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    /* Boundary prefix values */
    CHECK(parse_broadcast("192.168.1.1/32", &addr));
    inet_pton(AF_INET, "192.168.1.1", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    CHECK(parse_broadcast("192.168.1.1/0", &addr));
    inet_pton(AF_INET, "255.255.255.255", &expected);
    CHECK(addr.s_addr == expected.s_addr);

    /* Invalid */
    CHECK(!parse_broadcast("not-an-ip", &addr));
    CHECK(!parse_broadcast("192.168.1.1/33", &addr));
    CHECK(!parse_broadcast("/24", &addr));
    CHECK(!parse_broadcast("192.168.1.1/", &addr));
}

static int run_self_tests(void)
{
    tests_run = tests_failed = 0;

    if (!initialize_network()) {
        fprintf(stderr, "self-test: network init failed\n");
        return 1;
    }

    test_parse_mac();
    test_parse_port();
    test_parse_broadcast();

    net_cleanup();

    if (tests_failed == 0)
        printf("self-test: %d/%d passed\n", tests_run, tests_run);
    else
        printf("self-test: %d/%d passed, %d FAILED\n",
               tests_run - tests_failed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}

#endif /* WOL_SELF_TEST */


/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    struct cli opts;
    struct sockaddr_in dest;
    struct mac *macs = NULL;
    FILE *mac_file = NULL;
    SOCKET sock = INVALID_SOCKET;
    int net_up = 0;    /* set after initialize_network() succeeds; gates net_cleanup() */
    int file_count = 0;
    int cli_count, mac_count, i, all_ok = 1;
    int ret = 1;

#ifdef WOL_SELF_TEST
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return run_self_tests();
#endif

    if (!parse_cli(argc, argv, &opts)) {
        fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
        return 1;
    }
    /* ACT_HELP and ACT_VERSION return directly: no resources acquired yet. */
    if (opts.action == ACT_VERSION) { printf("wol %s\n", WOL_VERSION); return 0; }
    if (opts.action == ACT_HELP)    { print_help(argv[0]); return 0; }

    build_dest_addr(opts.broadcast_addr, opts.port, &dest);

    if (opts.mac_file_path != NULL) {
        mac_file = fopen(opts.mac_file_path, "r");
        if (mac_file == NULL) {
            fprintf(stderr, "Error: cannot open MAC file: %s\n", opts.mac_file_path);
            goto cleanup;
        }
        file_count = scan_mac_file(mac_file, opts.mac_file_path);
        if (file_count < 0) goto cleanup;
        if (file_count > 0) {
            rewind(mac_file);
        } else {
            fclose(mac_file);
            mac_file = NULL;
        }
    }

    cli_count = argc - opts.first_mac_index;
    mac_count = file_count + cli_count;

    if (mac_count == 0) {
        if (opts.mac_file_path != NULL)
            fprintf(stderr, "Error: no MAC addresses found in file: %s\n", opts.mac_file_path);
        else
            fprintf(stderr, "Error: no MAC address specified.\n");
        fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
        goto cleanup;
    }

    /* calloc rather than a VLA: MSVC does not support variable-length arrays. */
    macs = (struct mac *)calloc((size_t)mac_count, sizeof(*macs));
    if (macs == NULL) { fprintf(stderr, "Error: out of memory\n"); goto cleanup; }

    /* Validate CLI MACs before loading the file: they are fallible (parse_mac can
     * reject) and require no I/O, so catch bad input early. Stored after file MACs. */
    for (i = 0; i < cli_count; ++i) {
        int idx = file_count + i;
        const char *str = argv[opts.first_mac_index + i];
        if (!parse_mac(str, macs[idx].value)) {
            fprintf(stderr, "Error: invalid MAC address: %s\n", str);
            goto cleanup;
        }
        format_mac(macs[idx].value, macs[idx].text);
    }

    if (mac_file != NULL) {
        load_mac_file(mac_file, macs, 0);
        fclose(mac_file);
        mac_file = NULL;
    }

    if (!initialize_network()) goto cleanup;
    net_up = 1;

    if (!open_udp_socket(&sock)) goto cleanup;

    for (i = 0; i < mac_count; ++i) {
        int err = 0;
        if (send_magic_packet(sock, &dest, macs[i].value, &err)) {
            printf("WoL sent  ->  %s\n", macs[i].text);
        } else {
            if (err != 0) {
                char errbuf[256];
                net_error_str(err, errbuf, sizeof(errbuf));
                fprintf(stderr, "Failed    ->  %s  %s (%d)\n", macs[i].text, errbuf, err);
            } else {
                fprintf(stderr, "Failed    ->  %s  short send\n", macs[i].text);
            }
            all_ok = 0;
        }
    }
    ret = all_ok ? 0 : 1;

cleanup:
    if (sock != INVALID_SOCKET) net_close(sock);
    if (net_up) net_cleanup();
    free(macs);                  /* free(NULL) is defined as a no-op */
    if (mac_file != NULL) fclose(mac_file);
    return ret;
}
