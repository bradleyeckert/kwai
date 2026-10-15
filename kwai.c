/*
 * kwai: a line-mode terminal for a serial port, made for LiteForth boards.
 *
 *   kwai [-b baud] [--crlf] PORT          e.g. kwai COM15, kwai /dev/ttyACM0
 *
 * The console stays in its normal (cooked) line mode, so it does the
 * editing, echo and history (the up arrow, on Windows); each line typed is
 * sent to the port when Enter is pressed, ending in LF. A reader thread
 * prints what the port sends. Lines that start with ~ are commands for
 * kwai itself (~? lists them), including an XMODEM send.
 *
 * If the port goes away (a USB CDC device re-enumerating after a reflash),
 * kwai says so and reopens it as soon as it comes back. It also waits for
 * the port if it isn't there at startup.
 *
 * Builds with gcc or clang on Linux, macOS and MinGW, and with MSVC.
 */
#if defined(_WIN32) || defined(_WIN64)
#define KWAI_WINDOWS 1
#define _CRT_SECURE_NO_WARNINGS
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600         /* condition variables */
#endif
#include <windows.h>
#include <io.h>
#else
#define _DEFAULT_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define KWAI_VERSION "0.1"

/* ======================================================================
 * Threads, locks and time
 */
#ifdef KWAI_WINDOWS
typedef CRITICAL_SECTION lock_t;
typedef CONDITION_VARIABLE cond_t;
static void lock_init(lock_t* l) { InitializeCriticalSection(l); }
static void lock(lock_t* l) { EnterCriticalSection(l); }
static void unlock(lock_t* l) { LeaveCriticalSection(l); }
static void cond_init(cond_t* c) { InitializeConditionVariable(c); }
static void cond_signal(cond_t* c) { WakeAllConditionVariable(c); }
/* Waits up to ms milliseconds; returns 0 if signalled, 1 on timeout. */
static int cond_wait_ms(cond_t* c, lock_t* l, int ms) {
    return SleepConditionVariableCS(c, l, (DWORD)ms) ? 0 : 1;
}
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static uint64_t now_ms(void) { return GetTickCount64(); }
static void (*thread_fn)(void);
static DWORD WINAPI thread_trampoline(LPVOID arg) {
    (void)arg;
    thread_fn();
    return 0;
}
static int start_thread(void (*fn)(void)) {
    thread_fn = fn;
    HANDLE h = CreateThread(NULL, 0, thread_trampoline, NULL, 0, NULL);
    if (h == NULL) return -1;
    CloseHandle(h);
    return 0;
}
#else
typedef pthread_mutex_t lock_t;
typedef pthread_cond_t cond_t;
static void lock_init(lock_t* l) { pthread_mutex_init(l, NULL); }
static void lock(lock_t* l) { pthread_mutex_lock(l); }
static void unlock(lock_t* l) { pthread_mutex_unlock(l); }
static void cond_init(cond_t* c) { pthread_cond_init(c, NULL); }
static void cond_signal(cond_t* c) { pthread_cond_broadcast(c); }
static int cond_wait_ms(cond_t* c, lock_t* l, int ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct timespec ts;
    long long ns = (long long)tv.tv_usec * 1000 + (long long)(ms % 1000) * 1000000;
    ts.tv_sec = tv.tv_sec + ms / 1000 + (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    return pthread_cond_timedwait(c, l, &ts) ? 1 : 0;
}
static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void* thread_trampoline(void* fn) {
    void (*f)(void);
    memcpy(&f, &fn, sizeof f);
    f();
    return NULL;
}
static int start_thread(void (*fn)(void)) {
    pthread_t t;
    void* arg;
    memcpy(&arg, &fn, sizeof arg);
    if (pthread_create(&t, NULL, thread_trampoline, arg) != 0) return -1;
    pthread_detach(t);
    return 0;
}
#endif

/* ======================================================================
 * Console output
 *
 * Port output goes to stdout as it arrives. On a Windows console it is
 * converted from UTF-8 to UTF-16 and written with WriteConsoleW, so the
 * console's code page doesn't matter; a UTF-8 sequence split between two
 * reads is held back until it is complete. Elsewhere the bytes are written
 * as they are. kwai's own messages go to stderr, in brackets.
 */
static lock_t out_lock;

#ifdef KWAI_WINDOWS
static HANDLE out_console = INVALID_HANDLE_VALUE;   /* stdout, if a console */

static void console_init(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        out_console = h;
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
static void console_init(void) { }
#endif

/* Bytes of an incomplete UTF-8 sequence at the end of p[0..n). */
static int utf8_tail(const unsigned char* p, int n) {
    for (int k = 1; k <= 3 && k <= n; k++) {
        unsigned char c = p[n - k];
        if ((c & 0xC0) == 0x80) continue;           /* continuation byte */
        int need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        return (need > k) ? k : 0;
    }
    return 0;
}

/* Writes port output to stdout. Call with out_lock held. */
static void out_bytes(const unsigned char* p, int n) {
    static unsigned char carry[4];
    static int ncarry;
    unsigned char buf[4096 + 4];
    if (n > 4096) n = 4096;
    memcpy(buf, carry, (size_t)ncarry);
    memcpy(buf + ncarry, p, (size_t)n);
    int total = ncarry + n;
    int keep = utf8_tail(buf, total);
    int len = total - keep;
    memcpy(carry, buf + len, (size_t)keep);
    ncarry = keep;
    if (len == 0) return;
#ifdef KWAI_WINDOWS
    if (out_console != INVALID_HANDLE_VALUE) {
        static wchar_t wbuf[4096 + 4];
        int w = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf, len, wbuf, 4096 + 4);
        DWORD done;
        if (w > 0) WriteConsoleW(out_console, wbuf, (DWORD)w, &done, NULL);
        return;
    }
#endif
    fwrite(buf, 1, (size_t)len, stdout);
    fflush(stdout);
}

/* A message from kwai itself: "[kwai: ...]" on a line of its own. */
static void note(const char* fmt, ...) {
    va_list ap;
    lock(&out_lock);
    fputs("\n[kwai: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("]\n", stderr);
    fflush(stderr);
    unlock(&out_lock);
}

/* ======================================================================
 * The serial port
 *
 * port_open opens it raw, 8N1, with DTR and RTS on: the LiteForth board's
 * USB bridge only sends while DTR is on. port_read waits up to ms
 * milliseconds for input and returns the bytes read, 0 if none came, or -1
 * if the port has gone. port_write returns 0, or -1 if the port has gone.
 */
static char port_name[256];
static int port_baud = 115200;

#ifdef KWAI_WINDOWS
static HANDLE port = INVALID_HANDLE_VALUE;

static int port_open(void) {
    char path[300];
    snprintf(path, sizeof path, "\\\\.\\%s", port_name);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return -1; }
    dcb.BaudRate = (DWORD)port_baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return -1; }
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = MAXDWORD;               /* return what's there, */
    t.ReadTotalTimeoutMultiplier = MAXDWORD;        /* or wait for the first */
    t.ReadTotalTimeoutConstant = 20;                /* byte up to 20 ms */
    t.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h, &t);
    EscapeCommFunction(h, SETDTR);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    port = h;
    return 0;
}

static void port_close(void) {
    if (port != INVALID_HANDLE_VALUE) CloseHandle(port);
    port = INVALID_HANDLE_VALUE;
}

static int port_read(unsigned char* buf, int n, int ms) {
    (void)ms;                                       /* the timeouts above */
    DWORD got = 0;
    if (!ReadFile(port, buf, (DWORD)n, &got, NULL)) return -1;
    if (got == 0) {                                 /* nothing, or gone? */
        DWORD errors;
        COMSTAT st;
        if (!ClearCommError(port, &errors, &st)) return -1;
    }
    return (int)got;
}

static int port_write(const unsigned char* p, int n) {
    while (n > 0) {
        DWORD done = 0;
        if (!WriteFile(port, p, (DWORD)n, &done, NULL) || done == 0) return -1;
        p += done;
        n -= (int)done;
    }
    return 0;
}
#else
static int port = -1;

static speed_t baud_code(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
    default: return B115200;
    }
}

static int port_open(void) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) < 0) { close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD | HUPCL;            /* HUPCL: DTR on while open */
#ifdef CRTSCTS
    t.c_cflag &= ~CRTSCTS;
#endif
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, baud_code(port_baud));
    cfsetospeed(&t, baud_code(port_baud));
    if (tcsetattr(fd, TCSANOW, &t) < 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    port = fd;
    return 0;
}

static void port_close(void) {
    if (port >= 0) close(port);
    port = -1;
}

static int port_read(unsigned char* buf, int n, int ms) {
    struct pollfd p = { port, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    if (r < 0) return (errno == EINTR) ? 0 : -1;
    if (r == 0) return 0;
    if (p.revents & POLLIN) {
        ssize_t got = read(port, buf, (size_t)n);
        if (got > 0) return (int)got;
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1;                                  /* 0: hung up */
    }
    return -1;                                      /* POLLHUP, POLLERR */
}

static int port_write(const unsigned char* p, int n) {
    while (n > 0) {
        ssize_t done = write(port, p, (size_t)n);
        if (done < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                struct pollfd q = { port, POLLOUT, 0 };
                if (poll(&q, 1, 2000) <= 0 || (q.revents & (POLLERR | POLLHUP))) return -1;
                continue;
            }
            return -1;
        }
        p += done;
        n -= (int)done;
    }
    return 0;
}
#endif

/* ======================================================================
 * The reader thread
 *
 * It owns opening and closing the port. While connected, it reads the
 * port and either prints the bytes or, during an XMODEM transfer, queues
 * them for xm_getc. When a read or a write fails it closes the port and
 * tries to reopen it every RETRY_MS until it succeeds.
 */
#define RETRY_MS 300
#define RXQ_SIZE 8192                   /* a power of 2 */

static lock_t port_lock;                /* port handle: open, close, write */
static volatile int connected;
static volatile int port_broken;        /* a write failed: reopen */
static volatile int quitting;

static lock_t rxq_lock;
static cond_t rxq_cond;
static unsigned char rxq[RXQ_SIZE];
static unsigned rxq_head, rxq_tail;
static volatile int xm_active;          /* queue input instead of printing */

static void reader(void) {
    unsigned char buf[1024];
    int announced_wait = 0;
    while (!quitting) {
        if (!connected) {
            lock(&port_lock);
            int ok = (port_open() == 0);
            if (ok) { connected = 1; port_broken = 0; }
            unlock(&port_lock);
            if (ok) {
                note("%s %s", port_name, announced_wait ? "is back" : "connected; ~? for help");
                announced_wait = 0;
            } else {
                if (!announced_wait) note("waiting for %s", port_name);
                announced_wait = 1;
                sleep_ms(RETRY_MS);
            }
            continue;
        }
        int n = port_broken ? -1 : port_read(buf, (int)sizeof buf, 20);
        if (n < 0) {
            lock(&port_lock);
            port_close();
            connected = 0;
            unlock(&port_lock);
            if (!quitting) note("%s lost; waiting for it to come back", port_name);
            announced_wait = 1;
            sleep_ms(RETRY_MS);
            continue;
        }
        if (n == 0) continue;
        if (xm_active) {
            lock(&rxq_lock);
            for (int i = 0; i < n && rxq_head - rxq_tail < RXQ_SIZE; i++) {
                rxq[rxq_head++ & (RXQ_SIZE - 1)] = buf[i];
            }
            cond_signal(&rxq_cond);
            unlock(&rxq_lock);
        } else {
            lock(&out_lock);
            out_bytes(buf, n);
            unlock(&out_lock);
        }
    }
}

/* Sends bytes to the port. Returns 0, or -1 if it isn't connected. */
static int send_bytes(const void* p, int n) {
    int r = -1;
    lock(&port_lock);
    if (connected && !port_broken) {
        r = port_write((const unsigned char*)p, n);
        if (r) port_broken = 1;                     /* the reader reopens it */
    }
    unlock(&port_lock);
    return r;
}

/* ======================================================================
 * XMODEM send
 *
 * Original XMODEM with 1K packets: the receiver starts it with NAK
 * (8-bit checksum) or 'C' (CRC-16); each packet is STX (1024 bytes) or
 * SOH (128, for the last short piece), the block number and its
 * complement, the data padded with SUB, and the checksum or CRC; EOT ends
 * the transfer. LiteForth's `xmodem` asks for checksums and takes 1K
 * packets. Text the board prints before the first NAK is shown.
 */
#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1A

#define XM_START_MS   60000             /* wait for the receiver to start */
#define XM_REPLY_MS   10000             /* wait for ACK or NAK */
#define XM_RETRIES    10

/* The next byte from the port during a transfer, or -1 after ms. */
static int xm_getc(int ms) {
    int c = -1;
    uint64_t end = now_ms() + (uint64_t)ms;
    lock(&rxq_lock);
    while (rxq_head == rxq_tail) {
        uint64_t t = now_ms();
        if (t >= end || !connected) break;
        cond_wait_ms(&rxq_cond, &rxq_lock, (int)(end - t > 100 ? 100 : end - t));
    }
    if (rxq_head != rxq_tail) c = rxq[rxq_tail++ & (RXQ_SIZE - 1)];
    unlock(&rxq_lock);
    return c;
}

/* Ends a transfer: prints whatever was queued and goes back to printing. */
static void xm_end(void) {
    lock(&rxq_lock);
    lock(&out_lock);
    while (rxq_head != rxq_tail) {
        unsigned char c = rxq[rxq_tail++ & (RXQ_SIZE - 1)];
        out_bytes(&c, 1);
    }
    xm_active = 0;
    unlock(&out_lock);
    unlock(&rxq_lock);
}

static uint16_t crc16(const unsigned char* p, int n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static unsigned char* read_file(const char* path, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* data = (unsigned char*)malloc((size_t)(n > 0 ? n : 1));
    if (data && n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    *size = n;
    return data;
}

/* Sends a file by XMODEM. Returns 0 if the receiver took all of it. */
static int xmodem_send(const char* path) {
    long size = 0;
    unsigned char* data = read_file(path, &size);
    if (!data) { note("can't read %s", path); return -1; }
    if (!connected) { note("not connected"); free(data); return -1; }
    lock(&rxq_lock);
    rxq_head = rxq_tail = 0;
    xm_active = 1;
    unlock(&rxq_lock);

    /* the receiver's start: NAK or 'C'; show any text before it */
    int crc = -1;
    uint64_t end = now_ms() + XM_START_MS;
    while (crc < 0 && now_ms() < end) {
        int c = xm_getc(500);
        if (c == NAK) crc = 0;
        else if (c == 'C') crc = 1;
        else if (c == CAN) { note("the receiver cancelled"); goto fail; }
        else if (c >= 0) { unsigned char b = (unsigned char)c; lock(&out_lock); out_bytes(&b, 1); unlock(&out_lock); }
        if (!connected) break;
    }
    if (crc < 0) { note("no XMODEM receiver answered"); goto fail; }

    long packets = (size + 1023) / 1024, sent = 0;
    unsigned char pkt[3 + 1024 + 2];
    unsigned char blockno = 1;
    note("sending %s: %ld bytes, %s", path, size, crc ? "CRC" : "checksum");
    for (long pos = 0; pos < size || (size == 0 && pos == 0); ) {
        long left = size - pos;
        int len = (left > 128) ? 1024 : 128;
        pkt[0] = (len == 1024) ? STX : SOH;
        pkt[1] = blockno;
        pkt[2] = (unsigned char)~blockno;
        long take = (left < len) ? left : len;
        if (take > 0) memcpy(pkt + 3, data + pos, (size_t)take);
        memset(pkt + 3 + take, SUB, (size_t)(len - take));
        int plen = 3 + len;
        if (crc) {
            uint16_t c = crc16(pkt + 3, len);
            pkt[plen++] = (unsigned char)(c >> 8);
            pkt[plen++] = (unsigned char)c;
        } else {
            unsigned char sum = 0;
            for (int i = 0; i < len; i++) sum = (unsigned char)(sum + pkt[3 + i]);
            pkt[plen++] = sum;
        }
        int tries = 0, reply = -1;
        for (;;) {
            if (send_bytes(pkt, plen)) { note("the port went away"); goto fail; }
            reply = xm_getc(XM_REPLY_MS);
            while (reply >= 0 && reply != ACK && reply != NAK && reply != CAN) reply = xm_getc(XM_REPLY_MS);
            if (reply == ACK) break;
            if (reply == CAN) { note("the receiver cancelled at packet %ld", sent + 1); goto fail; }
            if (++tries >= XM_RETRIES) { note("packet %ld failed %d times", sent + 1, tries); goto cancel; }
        }
        pos += take;
        blockno++;
        sent++;
        lock(&out_lock);
        fprintf(stderr, "\r[kwai: %ld/%ld]", sent, packets > 0 ? packets : 1);
        fflush(stderr);
        unlock(&out_lock);
        if (size == 0) break;
    }
    for (int tries = 0; ; tries++) {
        unsigned char eot = EOT;
        if (send_bytes(&eot, 1)) { note("the port went away"); goto fail; }
        int reply = xm_getc(XM_REPLY_MS);
        while (reply >= 0 && reply != ACK && reply != NAK) reply = xm_getc(XM_REPLY_MS);
        if (reply == ACK) break;
        if (tries >= XM_RETRIES) { note("EOT not acknowledged"); goto fail; }
    }
    note("sent %s", path);
    free(data);
    xm_end();
    return 0;
cancel:
    {
        unsigned char can[3] = { CAN, CAN, CAN };
        send_bytes(can, 3);
    }
fail:
    free(data);
    xm_end();
    return -1;
}

/* ======================================================================
 * Console input: one line at a time, in the console's own line mode
 */
#ifdef KWAI_WINDOWS
static HANDLE in_console = INVALID_HANDLE_VALUE;    /* stdin, if a console */

/* Reads a line as UTF-8 without its line end. Returns 0, or -1 at the end. */
static int read_line(char* line, int size) {
    if (in_console == INVALID_HANDLE_VALUE) {
        DWORD mode;
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            in_console = h;
            SetConsoleMode(h, mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        } else {
            in_console = NULL;                      /* redirected: read bytes */
        }
    }
    if (in_console) {
        wchar_t w[1024];
        DWORD got = 0;
        if (!ReadConsoleW(in_console, w, 1023, &got, NULL) || got == 0) return -1;
        while (got > 0 && (w[got - 1] == L'\n' || w[got - 1] == L'\r')) got--;
        int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)got, line, size - 1, NULL, NULL);
        line[n > 0 ? n : 0] = 0;
        return 0;
    }
    if (!fgets(line, size, stdin)) return -1;
    line[strcspn(line, "\r\n")] = 0;
    return 0;
}
#else
static int read_line(char* line, int size) {
    if (!fgets(line, size, stdin)) return -1;
    line[strcspn(line, "\r\n")] = 0;
    return 0;
}
#endif

/* ======================================================================
 * Commands and the main loop
 */
static const char* eol = "\n";

static void help(void) {
    note("lines go to %s with %s; lines starting with ~ are for kwai:\n"
         "  ~x FILE   type `xmodem` on the board, then send FILE by XMODEM\n"
         "  ~s FILE   send FILE by XMODEM to a receiver that is already waiting\n"
         "  ~r        send Ctrl+X three times (restarts a LiteForth CH32H417 without its app)\n"
         "  ~~TEXT    send ~TEXT\n"
         "  ~?        this list\n"
         "  ~q        quit (so does Ctrl+C)",
         port_name, eol[0] == '\r' ? "CR LF" : "LF");
}

static const char* arg_of(const char* cmd) {
    while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    return cmd;
}

/* Runs a ~ command. Returns 1 to quit. */
static int command(const char* line) {
    char c = line[1];
    if (c == '~') {
        send_bytes(line + 1, (int)strlen(line + 1));
        send_bytes(eol, (int)strlen(eol));
    } else if (c == 'q' || c == 'Q') {
        return 1;
    } else if (c == 'r' || c == 'R') {
        static const unsigned char x3[3] = { 0x18, 0x18, 0x18 };
        if (send_bytes(x3, 3)) note("not connected");
    } else if ((c == 'x' || c == 'X' || c == 's' || c == 'S') && line[2] == ' ') {
        const char* path = arg_of(line);
        if (!*path) { note("which file?"); return 0; }
        if (c == 'x' || c == 'X') {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "xmodem%s", eol);
            if (send_bytes(cmd, (int)strlen(cmd))) { note("not connected"); return 0; }
        }
        xmodem_send(path);
    } else {
        help();
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "kwai " KWAI_VERSION ": a line-mode terminal for a serial port\n"
        "usage: kwai [-b baud] [--crlf] PORT     (COM15, /dev/ttyACM0, ...)\n"
        "  -b baud   bits per second (default 115200; USB CDC ignores it)\n"
        "  --crlf    end lines with CR LF instead of LF\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) port_baud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crlf")) eol = "\r\n";
        else if (argv[i][0] == '-') { usage(); return 2; }
        else snprintf(port_name, sizeof port_name, "%s", argv[i]);
    }
    if (!port_name[0] || port_baud <= 0) { usage(); return 2; }

    console_init();
    lock_init(&out_lock);
    lock_init(&port_lock);
    lock_init(&rxq_lock);
    cond_init(&rxq_cond);
    if (start_thread(reader)) { fprintf(stderr, "kwai: can't start the reader\n"); return 1; }

    char line[2048];
    while (read_line(line, (int)sizeof line) == 0) {
        if (line[0] == '~') {
            if (command(line)) break;
            continue;
        }
        if (send_bytes(line, (int)strlen(line)) || send_bytes(eol, (int)strlen(eol))) {
            note("not connected: line not sent");
        }
    }
    if (feof(stdin)) sleep_ms(500);                 /* redirected input: let output finish */
    quitting = 1;
    lock(&port_lock);
    port_close();
    connected = 0;
    unlock(&port_lock);
    return 0;
}
