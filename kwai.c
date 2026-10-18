/*
 * kwai: a line-mode terminal for a serial port, made for LiteForth boards.
 *
 *   kwai [-b baud] [-p ms] [--crlf] PORT  e.g. kwai COM15, kwai /dev/ttyACM0
 *
 * The console stays in its normal (cooked) line mode, so it does the
 * editing, echo and history (the up arrow, on Windows); each line typed is
 * sent to the port when Enter is pressed, ending in LF; a pasted block goes
 * a line at a time, each after the board's `ok>` for the last (or 100 ms).
 * A reader thread prints what the port sends. Lines that start with ~ are
 * commands for kwai itself (~? lists them), including a YMODEM send.
 *
 * If the port goes away (a USB CDC device re-enumerating after a reflash),
 * kwai says so and reopens it as soon as it comes back. It also waits for
 * the port if it isn't there at startup.
 *
 * Builds with gcc or clang on Linux, macOS and MinGW, and with MSVC.
 */
#if defined(_WIN32) || defined(_WIN64)
#define KWAI_WINDOWS 1
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS     /* fopen, ... (build.bat also defines it) */
#endif
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
#include <glob.h>
#include <dirent.h>
#include <limits.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define KWAI_VERSION "0.2"

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

static void prompt_scan(const unsigned char* p, int n);

/* -o FILE: the transcript. Port output goes in as it comes, with the lines
 * sent and kwai's messages. Write to it with out_lock held. */
static FILE* log_file;

static void log_bytes(const void* p, size_t n) {
    if (!log_file) return;
    fwrite(p, 1, n, log_file);
    fflush(log_file);
}

/* Writes port output to stdout. Call with out_lock held. */
static void out_bytes(const unsigned char* p, int n) {
    static unsigned char carry[4];
    static int ncarry;
    unsigned char buf[4096 + 4];
    if (n > 4096) n = 4096;
    prompt_scan(p, n);
    log_bytes(p, (size_t)n);
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
    if (log_file) {
        fputs("\n[kwai: ", log_file);
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
        fputs("]\n", log_file);
        fflush(log_file);
    }
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
 * port and either prints the bytes or, during a YMODEM transfer, queues
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
        /* xm_active is checked with rxq_lock held: xm_end may clear it
         * between a check and the lock, and the bytes would be queued
         * with nothing left to print them */
        lock(&rxq_lock);
        if (xm_active) {
            for (int i = 0; i < n && rxq_head - rxq_tail < RXQ_SIZE; i++) {
                rxq[rxq_head++ & (RXQ_SIZE - 1)] = buf[i];
            }
            cond_signal(&rxq_cond);
        } else {
            lock(&out_lock);                        /* same order as xm_end */
            out_bytes(buf, n);
            unlock(&out_lock);
        }
        unlock(&rxq_lock);
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
 * Pacing lines, and spotting errors
 *
 * A pasted block arrives as lines faster than the board can take them, so
 * before sending a line kwai waits for the previous one's `ok>` prompt,
 * but no longer than pace_ms after it was sent (a board that doesn't
 * prompt gets no `ok>`). A line typed by hand comes long after that, so it
 * isn't held up. -p 0 turns this off.
 *
 * LiteForth still prompts after an error, so the prompt doesn't say whether
 * a line worked. Its errors do stand out: a line ending in ` ?` (an
 * unknown word, `foo ?`) or one with `Error: ior=` in it. prompt_scan
 * watches the port output for those and for `ok>`, skipping escape
 * sequences (the error is in red), so line_result can say how the last
 * line sent went.
 */
static const char* eol = "\n";        /* --crlf: "\r\n" */
static int pace_ms = 100;
static lock_t prompt_lock;
static cond_t prompt_cond;
static int prompt_seen;                 /* `ok>` came since the last line */
static int prompt_ever;                 /* the board has prompted */
static int line_error;                  /* error text since the last line */
static int prompt_awaited;              /* a line was sent: wait for it */
static uint64_t line_sent_ms;

/* Watches port output for `ok>` and error text. Called from out_bytes, so
 * the text is caught even across reads. */
static void prompt_scan(const unsigned char* p, int n) {
    static const char ok[] = "ok>", err[] = "Error: ior=";
    static int ok_match, err_match, esc;
    static unsigned char last, before_last;  /* the line's last two chars */
    int seen = 0, error = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }   /* ESC x */
        if (esc == 2) { if (c >= 0x40 && c <= 0x7E) esc = 0; continue; }
        if (c == 0x1B) { esc = 1; continue; }
        if (c == '\r' || c == '\n') {
            if (last == '?' && before_last == ' ') error = 1;
            last = before_last = 0;
        } else {
            before_last = last;
            last = c;
        }
        if (c == (unsigned char)ok[ok_match]) ok_match++;
        else ok_match = (c == (unsigned char)ok[0]);
        if (ok_match == 3) { seen = 1; ok_match = 0; }
        if (c == (unsigned char)err[err_match]) err_match++;
        else err_match = (c == (unsigned char)err[0]);
        if (err[err_match] == 0) { error = 1; err_match = 0; }
    }
    if (seen || error) {
        lock(&prompt_lock);
        if (error) line_error = 1;
        if (seen) prompt_seen = prompt_ever = 1;
        cond_signal(&prompt_cond);
        unlock(&prompt_lock);
    }
}

/* Waits for the last line sent to finish: its `ok>`, or ms after it was
 * sent. Returns 1 if it gave an error, 0 if not (or no line is waiting),
 * and leaves it waiting. */
static int line_result(int ms) {
    lock(&prompt_lock);
    if (prompt_awaited && ms > 0) {
        uint64_t end = line_sent_ms + (uint64_t)ms;
        for (;;) {
            uint64_t t = now_ms();
            if (prompt_seen || t >= end || !connected) break;
            cond_wait_ms(&prompt_cond, &prompt_lock, (int)(end - t));
        }
    }
    int r = prompt_awaited && line_error;
    unlock(&prompt_lock);
    return r;
}

/* The longest ~f waits for each line's `ok>`: long enough for a line that
 * writes flash. Only once the board has prompted, so a board that doesn't
 * still gets pace_ms. */
#define FILE_LINE_MS 2000

static int line_wait_ms(int file) {
    int ms = pace_ms;
    if (file && pace_ms > 0) {
        lock(&prompt_lock);
        if (prompt_ever) ms = FILE_LINE_MS;
        unlock(&prompt_lock);
    }
    return ms;
}

/* Sends a line and its line end, as is: no waiting. Returns 0, or -1 if
 * it isn't connected. With echo, the line is also shown, as if typed. */
static int send_line_now(const char* text, int echo) {
    lock(&prompt_lock);
    prompt_seen = 0;
    line_error = 0;
    prompt_awaited = 0;
    unlock(&prompt_lock);
    int r = send_bytes(text, (int)strlen(text));
    if (r == 0) r = send_bytes(eol, (int)strlen(eol));
    if (r == 0) {
        lock(&prompt_lock);
        prompt_awaited = 1;
        line_sent_ms = now_ms();
        unlock(&prompt_lock);
        lock(&out_lock);
        if (echo) { fputs(text, stdout); fputc('\n', stdout); fflush(stdout); }
        log_bytes(text, strlen(text));
        log_bytes("\n", 1);
        unlock(&out_lock);
    }
    return r;
}

/* Sends a line after the last one has finished (line_result). Returns 0,
 * or -1 if it isn't connected. */
static int send_line(const char* text) {
    line_result(pace_ms);
    return send_line_now(text, 0);
}

/* ~f FILE: sends a text file a line at a time, each after the last one's
 * `ok>`, showing each one. Stops at the first line that gives an error. */
static int send_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { note("can't read %s", path); return -1; }
    char line[2048];
    long n = 0;
    int r = 0;
    while (fgets(line, (int)sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (n > 0 && line_result(line_wait_ms(1))) {
            note("%s line %ld gave an error; stopped there", path, n);
            r = -1;
            break;
        }
        if (send_line_now(line, 1)) { note("not connected: %s stopped at line %ld", path, n + 1); r = -1; break; }
        n++;
    }
    if (r == 0) {
        if (n > 0 && line_result(line_wait_ms(1))) {
            note("%s line %ld gave an error", path, n);
            r = -1;
        } else {
            note("sent %s: %ld lines", path, n);
        }
    }
    fclose(f);
    return r;
}

/* ======================================================================
 * YMODEM send
 *
 * YMODEM batch with CRC-16 and 1K packets: the receiver asks with 'C'.
 * Packet 0 carries the file's name, a NUL and its length in decimal; the
 * receiver ACKs it and asks again with 'C'. Each data packet is STX
 * (1024 bytes) or SOH (128, for a short last piece), the block number and
 * its complement, the data padded with SUB, and the CRC (big-endian). EOT
 * ends the file (the receiver NAKs the first one and ACKs the second), and
 * an empty packet 0 ends the batch. The length tells the receiver which
 * bytes are padding, so binary files arrive exactly. Text the board
 * prints before the first 'C' is shown; LiteForth's `ymodem` prints its
 * reports after the batch.
 */
#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1A

#define CRC 'C'

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

/* Builds packet blockno of len bytes (128 or 1024) from take bytes of src,
   padded with pad, with its CRC. Returns the packet's length. */
static int make_packet(unsigned char* pkt, unsigned char blockno, const unsigned char* src,
                       long take, int len, unsigned char pad) {
    pkt[0] = (len == 1024) ? STX : SOH;
    pkt[1] = blockno;
    pkt[2] = (unsigned char)~blockno;
    if (take > 0) memcpy(pkt + 3, src, (size_t)take);
    memset(pkt + 3 + take, pad, (size_t)(len - take));
    uint16_t c = crc16(pkt + 3, len);
    pkt[3 + len] = (unsigned char)(c >> 8);
    pkt[4 + len] = (unsigned char)c;
    return 5 + len;
}

/* Sends a packet until it's ACKed. Returns 0, or -1 (cancelled, gone, or
   too many tries: the message is shown). */
static int send_packet(const unsigned char* pkt, int plen, const char* what) {
    for (int tries = 0; ; ) {
        if (send_bytes(pkt, plen)) { note("the port went away"); return -1; }
        int reply = xm_getc(XM_REPLY_MS);
        while (reply >= 0 && reply != ACK && reply != NAK && reply != CAN) reply = xm_getc(XM_REPLY_MS);
        if (reply == ACK) return 0;
        if (reply == CAN) { note("the receiver cancelled at %s", what); return -1; }
        if (++tries >= XM_RETRIES) { note("%s failed %d times", what, tries); return -2; }
    }
}

/* Waits for the receiver's 'C'; shows other text. Returns 0 or -1. */
static int await_c(int ms, const char* why) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (now_ms() < end && connected) {
        int c = xm_getc(500);
        if (c == CRC) return 0;
        if (c == CAN) { note("the receiver cancelled"); return -1; }
        if (c == NAK) { note("the receiver asks for XMODEM checksums; kwai sends YMODEM"); return -1; }
        if (c >= 0) { unsigned char b = (unsigned char)c; lock(&out_lock); out_bytes(&b, 1); unlock(&out_lock); }
    }
    note("%s", why);
    return -1;
}

static const char* base_name(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    }
    return b;
}

/* Sends a file by YMODEM, as a batch of one. Returns 0 if the receiver
   took all of it. */
static int ymodem_send(const char* path) {
    long size = 0;
    unsigned char* data = read_file(path, &size);
    if (!data) { note("can't read %s", path); return -1; }
    if (!connected) { note("not connected"); free(data); return -1; }
    lock(&rxq_lock);
    rxq_head = rxq_tail = 0;
    xm_active = 1;
    unlock(&rxq_lock);

    unsigned char pkt[5 + 1024];
    int plen, r;
    if (await_c(XM_START_MS, "no YMODEM receiver answered")) goto fail;

    /* packet 0: the name and the length */
    unsigned char info[128];
    const char* name = base_name(path);
    memset(info, 0, sizeof info);
    int n = snprintf((char*)info, 100, "%s", name);
    if (n > 99) n = 99;
    snprintf((char*)info + n + 1, sizeof info - (size_t)n - 1, "%ld", size);
    plen = make_packet(pkt, 0, info, 128, 128, 0);
    note("sending %s: %ld bytes", path, size);
    r = send_packet(pkt, plen, "the header");
    if (r == -2) goto cancel;
    if (r) goto fail;
    if (await_c(XM_REPLY_MS, "the receiver didn't ask for the data")) goto fail;

    long packets = (size + 1023) / 1024, sent = 0;
    unsigned char blockno = 1;
    for (long pos = 0; pos < size; ) {
        long left = size - pos;
        int len = (left > 128) ? 1024 : 128;
        long take = (left < len) ? left : len;
        plen = make_packet(pkt, blockno, data + pos, take, len, SUB);
        char what[32];
        snprintf(what, sizeof what, "packet %ld", sent + 1);
        r = send_packet(pkt, plen, what);
        if (r == -2) goto cancel;
        if (r) goto fail;
        pos += take;
        blockno++;
        sent++;
        lock(&out_lock);
        fprintf(stderr, "\r[kwai: %ld/%ld]", sent, packets);
        fflush(stderr);
        unlock(&out_lock);
    }
    /* EOT: NAKed once, then ACKed (an ACK the first time is fine too) */
    for (int tries = 0; ; tries++) {
        unsigned char eot = EOT;
        if (send_bytes(&eot, 1)) { note("the port went away"); goto fail; }
        int reply = xm_getc(XM_REPLY_MS);
        while (reply >= 0 && reply != ACK && reply != NAK) reply = xm_getc(XM_REPLY_MS);
        if (reply == ACK) break;
        if (tries >= XM_RETRIES) { note("EOT not acknowledged"); goto fail; }
    }
    /* the end of the batch: an empty packet 0 */
    if (await_c(XM_REPLY_MS, "the receiver didn't ask for the next file")) goto fail;
    memset(info, 0, sizeof info);
    plen = make_packet(pkt, 0, info, 128, 128, 0);
    r = send_packet(pkt, plen, "the end of the batch");
    if (r == -2) goto cancel;
    if (r) goto fail;
    note("sent %s", path);
    free(data);
    xm_end();
    return 0;
cancel:
    {
        unsigned char can[2] = { CAN, CAN };    /* two: three would be Ctrl+X x3 */
        send_bytes(can, 2);
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
 * Listing the serial ports
 *
 * Windows: the registry's HARDWARE\DEVICEMAP\SERIALCOMM, which maps each
 * driver device to its COM name (a USB CDC port, like the LiteForth
 * board's, is \Device\USBSERnnn). Linux: /dev/serial/by-id (names with
 * the maker, product and serial number) and the ttyACM, ttyUSB and ttyAMA
 * devices. macOS: /dev/cu.*.
 *
 * A USB port gets a second line with its VID:PID and the device
 * descriptor's Manufacturer, Product and Serial Number strings (from the
 * hub on Windows, from sysfs on Linux).
 */
typedef struct {
    int vid, pid;               /* -1: not a USB device */
    char man[128], prod[128], serial[128];
} usb_info_t;

/* Prints a USB port's second line: VID:PID and the descriptor strings */
static void show_usb(const usb_info_t* u, int indent) {
    if (u->vid < 0) return;
    printf("%*sUSB %04x:%04x", indent, "", u->vid, u->pid);
    if (u->man[0]) printf(", manufacturer \"%s\"", u->man);
    if (u->prod[0]) printf(", product \"%s\"", u->prod);
    if (u->serial[0]) printf(", serial \"%s\"", u->serial);
    printf("\n");
}

#ifdef KWAI_WINDOWS
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#endif
#include <setupapi.h>
#include <cfgmgr32.h>

/* USB descriptor strings. Windows keeps only some of them (the serial
 * number, when there is one, as the last part of the device ID), so kwai
 * does what USBView does: it finds the hub port the device is plugged
 * into and asks the hub for the descriptors. That doesn't disturb the
 * device or need administrator rights. The GUIDs and the IOCTL are spelled
 * out so no DDK header is needed. */
static const GUID kwai_guid_ports =     /* GUID_DEVCLASS_PORTS */
    { 0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
static const GUID kwai_guid_usb_hub =   /* GUID_DEVINTERFACE_USB_HUB */
    { 0xf18a0e88, 0xc30c, 0x11d0, { 0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8 } };
#define KWAI_IOCTL_USB_GET_DESCRIPTOR 0x220410  /* IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION */
#define KWAI_DESC_REQ 12        /* USB_DESCRIPTOR_REQUEST: port (4), setup packet (8) */

/* Asks the hub for descriptor (type, index) of the device on hub port hubport. Returns
 * its length, 0 if it fails. */
static int hub_descriptor(HANDLE hub, ULONG hubport, int type, int index, int lang,
                          unsigned char* out, int max) {
    unsigned char buf[KWAI_DESC_REQ + 255];
    DWORD got = 0;
    if (max > 255) max = 255;
    memset(buf, 0, sizeof buf);
    memcpy(buf, &hubport, 4);
    buf[4] = 0x80;                          /* device to host, standard */
    buf[5] = 6;                             /* GET_DESCRIPTOR */
    buf[6] = (unsigned char)index;          /* wValue: type << 8 | index */
    buf[7] = (unsigned char)type;
    buf[8] = (unsigned char)lang;           /* wIndex: language ID */
    buf[9] = (unsigned char)(lang >> 8);
    buf[10] = (unsigned char)max;           /* wLength */
    buf[11] = 0;
    if (!DeviceIoControl(hub, KWAI_IOCTL_USB_GET_DESCRIPTOR, buf, KWAI_DESC_REQ + (DWORD)max,
                         buf, KWAI_DESC_REQ + (DWORD)max, &got, NULL)) return 0;
    if (got <= KWAI_DESC_REQ + 2 || buf[KWAI_DESC_REQ + 1] != type) return 0;
    int n = (int)got - KWAI_DESC_REQ;
    if (buf[KWAI_DESC_REQ] < n) n = buf[KWAI_DESC_REQ];   /* bLength */
    memcpy(out, buf + KWAI_DESC_REQ, (size_t)n);
    return n;
}

/* String descriptor index as UTF-8 in out ("" if there is none) */
static void hub_string(HANDLE hub, ULONG hubport, int index, int lang, char* out, int max) {
    unsigned char d[255];
    wchar_t w[128];
    out[0] = 0;
    if (index == 0) return;
    int n = hub_descriptor(hub, hubport, 3, index, lang, d, (int)sizeof d);
    int nw = n > 2 ? (n - 2) / 2 : 0;
    for (int i = 0; i < nw; i++) w[i] = (wchar_t)(d[2 + 2 * i] | d[3 + 2 * i] << 8);
    int len = nw ? WideCharToMultiByte(CP_UTF8, 0, w, nw, out, max - 1, NULL, NULL) : 0;
    out[len > 0 ? len : 0] = 0;
}

/* Fills info from the hub the USB device dev is plugged into */
static void usb_from_hub(DEVINST dev, usb_info_t* info) {
    ULONG hubport = 0, size = sizeof hubport;
    DEVINST hubdev;
    char hubid[MAX_DEVICE_ID_LEN];
    if (CM_Get_DevNode_Registry_PropertyA(dev, CM_DRP_ADDRESS, NULL, &hubport, &size, 0) != CR_SUCCESS
        || CM_Get_Parent(&hubdev, dev, 0) != CR_SUCCESS
        || CM_Get_Device_IDA(hubdev, hubid, sizeof hubid, 0) != CR_SUCCESS) return;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeA(&len, (LPGUID)&kwai_guid_usb_hub, hubid,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2) return;
    char* paths = (char*)malloc(len);
    if (!paths) return;
    if (CM_Get_Device_Interface_ListA((LPGUID)&kwai_guid_usb_hub, hubid, paths, len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS && paths[0]) {
        HANDLE hub = CreateFileA(paths, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hub != INVALID_HANDLE_VALUE) {
            unsigned char d[18], langs[255];
            if (hub_descriptor(hub, hubport, 1, 0, 0, d, (int)sizeof d) >= 18) {
                int lang = 0x0409;
                if (hub_descriptor(hub, hubport, 3, 0, 0, langs, (int)sizeof langs) >= 4)
                    lang = langs[2] | langs[3] << 8;        /* the first language */
                info->vid = d[8] | d[9] << 8;
                info->pid = d[10] | d[11] << 8;
                hub_string(hub, hubport, d[14], lang, info->man, (int)sizeof info->man);
                hub_string(hub, hubport, d[15], lang, info->prod, (int)sizeof info->prod);
                hub_string(hub, hubport, d[16], lang, info->serial, (int)sizeof info->serial);
            }
            CloseHandle(hub);
        }
    }
    free(paths);
}

/* USB details of COM port com, from the device ID (VID, PID, serial) and
 * then from the hub. info->vid < 0 if it isn't a USB device. */
static void usb_info(const char* com, usb_info_t* info) {
    memset(info, 0, sizeof *info);
    info->vid = -1;
    HDEVINFO set = SetupDiGetClassDevsA(&kwai_guid_ports, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        char name[64];
        DWORD n = sizeof name - 1, type;
        HKEY key = SetupDiOpenDevRegKey(set, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        LONG r = RegQueryValueExA(key, "PortName", NULL, &type, (BYTE*)name, &n);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        name[n] = 0;
        if (_stricmp(name, com)) continue;
        /* Walk up to the USB device: USB\VID_1A86&PID_FE0C\serial. A port
         * of a composite device hangs off an interface (...&MI_00\...). */
        DEVINST dev = dd.DevInst;
        for (int up = 0; up < 4; up++) {
            char id[MAX_DEVICE_ID_LEN];
            if (CM_Get_Device_IDA(dev, id, sizeof id, 0) != CR_SUCCESS) break;
            if (!_strnicmp(id, "USB\\", 4) && !strstr(id, "&MI_")) {
                unsigned vid, pid;
                if (sscanf(id + 4, "VID_%4x&PID_%4x", &vid, &pid) == 2) {
                    const char* sn = strchr(id + 4, '\\');
                    info->vid = (int)vid;
                    info->pid = (int)pid;
                    if (sn && !strchr(sn, '&'))     /* Windows made up IDs with & */
                        snprintf(info->serial, sizeof info->serial, "%s", sn + 1);
                    usb_from_hub(dev, info);
                }
                break;
            }
            if (CM_Get_Parent(&dev, dev, 0) != CR_SUCCESS) break;
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

typedef struct { int num; char com[64]; char dev[200]; } com_t;

static int com_order(const void* a, const void* b) {
    return ((const com_t*)a)->num - ((const com_t*)b)->num;
}

static int list_ports(void) {
    HKEY key;
    com_t ports[256];
    int n = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0; n < 256; i++) {
            char dev[200];
            BYTE com[64];
            DWORD ndev = sizeof dev, ncom = sizeof com - 1, type;
            if (RegEnumValueA(key, i, dev, &ndev, NULL, &type, com, &ncom) != ERROR_SUCCESS) break;
            if (type != REG_SZ) continue;
            com[ncom] = 0;
            snprintf(ports[n].com, sizeof ports[n].com, "%s", (char*)com);
            snprintf(ports[n].dev, sizeof ports[n].dev, "%s", dev);
            ports[n].num = atoi(ports[n].com + (strncmp(ports[n].com, "COM", 3) ? 0 : 3));
            n++;
        }
        RegCloseKey(key);
    }
    qsort(ports, (size_t)n, sizeof ports[0], com_order);
    for (int i = 0; i < n; i++) {
        printf("  %-8s %s%s\n", ports[i].com, ports[i].dev,
               strstr(ports[i].dev, "USBSER") ? "  (USB CDC)" : "");
        usb_info_t u;
        usb_info(ports[i].com, &u);
        show_usb(&u, 11);
    }
    if (n == 0) printf("  no serial ports\n");
    fflush(stdout);
    return n;
}
#else
/* Reads the first line of file dir/name into out ("" if there is none) */
static void read_line_file(const char* dir, const char* name, char* out, int max) {
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    out[0] = 0;
    FILE* f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, max, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

/* USB details of tty dev (/dev/ttyACM0) from sysfs: the USB device is the
 * first directory above /sys/class/tty/ttyACM0/device with an idVendor.
 * KWAI_SYSFS replaces /sys (for testing). */
static void usb_info(const char* dev, usb_info_t* info) {
    char path[PATH_MAX + 64], dir[PATH_MAX], hex[16];
    const char* sys = getenv("KWAI_SYSFS");
    const char* base = strrchr(dev, '/');
    memset(info, 0, sizeof *info);
    info->vid = -1;
    snprintf(path, sizeof path, "%s/class/tty/%s/device", sys ? sys : "/sys", base ? base + 1 : dev);
    if (!realpath(path, dir)) return;
    for (int up = 0; up < 4; up++) {
        read_line_file(dir, "idVendor", hex, (int)sizeof hex);
        if (hex[0]) {
            info->vid = (int)strtol(hex, NULL, 16);
            read_line_file(dir, "idProduct", hex, (int)sizeof hex);
            info->pid = (int)strtol(hex, NULL, 16);
            read_line_file(dir, "manufacturer", info->man, (int)sizeof info->man);
            read_line_file(dir, "product", info->prod, (int)sizeof info->prod);
            read_line_file(dir, "serial", info->serial, (int)sizeof info->serial);
            return;
        }
        char* slash = strrchr(dir, '/');
        if (!slash || slash == dir) return;
        *slash = 0;
    }
}

static int list_ports(void) {
    int n = 0;
    usb_info_t u;
    char shown[64][PATH_MAX];
    int nshown = 0;
    DIR* d = opendir("/dev/serial/by-id");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char link[PATH_MAX + 32], target[PATH_MAX], real[PATH_MAX + 8];
            snprintf(link, sizeof link, "/dev/serial/by-id/%s", e->d_name);
            ssize_t len = readlink(link, target, sizeof target - 1);
            if (len <= 0) continue;
            target[len] = 0;
            const char* base = strrchr(target, '/');    /* udev: ../../ttyACM0 */
            snprintf(real, sizeof real, "/dev/%s", base ? base + 1 : target);
            printf("  %-16s %s\n", real, e->d_name);
            usb_info(real, &u);
            show_usb(&u, 19);
            if (nshown < 64) snprintf(shown[nshown++], sizeof shown[0], "%.*s", PATH_MAX - 1, real);
            n++;
        }
        closedir(d);
    }
    static const char* patterns[] = { "/dev/ttyACM*", "/dev/ttyUSB*", "/dev/ttyAMA*", "/dev/cu.*" };
    for (size_t p = 0; p < sizeof patterns / sizeof patterns[0]; p++) {
        glob_t g;
        if (glob(patterns[p], 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            int dup = 0;
            for (int k = 0; k < nshown; k++) if (!strcmp(shown[k], g.gl_pathv[i])) dup = 1;
            if (dup) continue;
            printf("  %s\n", g.gl_pathv[i]);
            usb_info(g.gl_pathv[i], &u);
            show_usb(&u, 4);
            n++;
        }
        globfree(&g);
    }
    if (n == 0) printf("  no serial ports (ttyACM, ttyUSB, ttyAMA, cu.*)\n");
    fflush(stdout);
    return n;
}
#endif

/* ======================================================================
 * Commands and the main loop
 */

static void help(void) {
    note("lines go to %s with %s; lines starting with ~ are for kwai:\n"
         "  ~x FILE   type `0 ymodem` on the board, then send source FILE into blocks by YMODEM\n"
         "  ~x FILE N type `N ymodem`, then send binary FILE from byte 32 of block N\n"
         "  ~s FILE   send FILE by YMODEM to a receiver that is already waiting\n"
         "  ~f FILE   send FILE's lines, each after the last one's ok>; stop at an error\n"
         "  ~r        send Ctrl+X three times (restarts a LiteForth CH32H417 without its app)\n"
         "  ~l        list the serial ports\n"
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
        if (send_line(line + 1)) note("not connected");
    } else if (c == 'q' || c == 'Q') {
        return 1;
    } else if (c == 'l' || c == 'L') {
        lock(&out_lock);
        printf("\n");
        list_ports();
        unlock(&out_lock);
    } else if (c == 'r' || c == 'R') {
        static const unsigned char x3[3] = { 0x18, 0x18, 0x18 };
        if (send_bytes(x3, 3)) note("not connected");
    } else if ((c == 'x' || c == 'X' || c == 's' || c == 'S') && line[2] == ' ') {
        char path[2048];
        snprintf(path, sizeof path, "%s", arg_of(line));
        size_t n = strlen(path);
        while (n && path[n - 1] == ' ') path[--n] = '\0';
        if (!n) { note("which file?"); return 0; }
        unsigned long block = 0;
        char* last = strrchr(path, ' ');            /* ~x FILE N: a block number */
        if (last && last[1] && strspn(last + 1, "0123456789") == strlen(last + 1)) {
            block = strtoul(last + 1, NULL, 10);
            while (last > path && last[-1] == ' ') last--;
            *last = '\0';
        }
        if (c == 'x' || c == 'X') {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "%lu ymodem", block);
            if (send_line(cmd)) { note("not connected"); return 0; }
        }
        ymodem_send(path);
    } else if ((c == 'f' || c == 'F') && line[2] == ' ') {
        const char* path = arg_of(line);
        if (!*path) { note("which file?"); return 0; }
        send_file(path);
    } else {
        help();
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "kwai " KWAI_VERSION ": a line-mode terminal for a serial port\n"
        "usage: kwai [-b baud] [-p ms] [-o FILE] [--crlf] PORT   (COM15, /dev/ttyACM0, ...)\n"
        "       kwai -l                                    list the serial ports\n"
        "  -b baud   bits per second (default 115200; USB CDC ignores it)\n"
        "  -p ms     wait up to ms for `ok>` between lines (default 100; 0: don't)\n"
        "  -o FILE   add a transcript of the session to FILE\n"
        "  --crlf    end lines with CR LF instead of LF\n");
}

/* A line that read_line returned within QUEUED_MS was already waiting: a
 * line of a paste (or of redirected input), not one typed. */
#define QUEUED_MS 20

int main(int argc, char** argv) {
    const char* log_path = NULL;
    console_init();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) port_baud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crlf")) eol = "\r\n";
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pace_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) { list_ports(); return 0; }
        else if (argv[i][0] == '-') { usage(); return 2; }
        else snprintf(port_name, sizeof port_name, "%s", argv[i]);
    }
    if (!port_name[0] || port_baud <= 0 || pace_ms < 0) {
        usage();
        fprintf(stderr, "serial ports:\n");
        fflush(stderr);
        list_ports();
        return 2;
    }

    if (log_path) {
        log_file = fopen(log_path, "ab");
        if (!log_file) { fprintf(stderr, "kwai: can't write %s\n", log_path); return 1; }
        char when[64];
        time_t t = time(NULL);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(log_file, "\n[kwai %s: %s, %s]\n", KWAI_VERSION, port_name, when);
        fflush(log_file);
    }
    lock_init(&out_lock);
    lock_init(&port_lock);
    lock_init(&rxq_lock);
    cond_init(&rxq_cond);
    lock_init(&prompt_lock);
    cond_init(&prompt_cond);
    if (start_thread(reader)) { fprintf(stderr, "kwai: can't start the reader\n"); return 1; }

    /* When a pasted line gives an error, the rest of the paste is dropped:
     * the lines read without waiting. A line typed after that goes. */
    char line[2048];
    int dropping = 0;
    for (;;) {
        uint64_t asked = now_ms();
        if (read_line(line, (int)sizeof line)) break;
        int queued = now_ms() - asked < QUEUED_MS;
        if (!queued) dropping = 0;
        else if (!dropping && line_result(pace_ms)) {
            dropping = 1;
            note("a pasted line gave an error; the rest of the paste isn't sent");
        }
        if (dropping) continue;
        if (line[0] == '~') {
            if (command(line)) break;
            continue;
        }
        if (send_line(line)) {
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
