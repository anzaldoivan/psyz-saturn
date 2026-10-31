// Saturn (libyaul) log, pad, video and libyaul startup endpoints.
#include <yaul.h>
#include <psyz.h>
#include <psyz/input.h>
#include <psyz/log.h>
#include <psyz/video.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "saturn_pad.h"

// psyz/include/psyz/system.h:27 `#define write psyz_write` would rename the
// libyaul FILE member in the __stdout_FILE/__stderr_FILE initializers below;
// this file never calls write().
#undef write

#define LOGSZ 4096

LOG_LEVEL psyz_logLevel = LOG_LEVEL_I;

// RAM ring buffer: the host reads it out of a save state
char psyz_saturn_log[LOGSZ];
volatile unsigned psyz_saturn_log_pos;

void psyz_log(unsigned int level, const char* file, unsigned int line,
              const char* func, const char* fmt, ...) {
    char msg[192], out[256];
    va_list ap;
    int n, i;
    (void)file;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    n = snprintf(out, sizeof(out), "L%u %s:%u %s\n", level, func, line, msg);
    if (n >= (int)sizeof(out)) {
        n = sizeof(out) - 1;
    }
    for (i = 0; i < n; i++) {
        psyz_saturn_log[psyz_saturn_log_pos++ % LOGSZ] = out[i];
    }
}

// libyaul stdio.c:3-5 defines __stdout_FILE/__stderr_FILE as weak zero FILEs;
// putc.c and vfprintf.c:61-63 call f->write unbuffered, so a frontend printf
// jumps to address 0. These strong definitions route the
// bytes into the RAM ring like psyz_log. NUL is dropped: vfprintf.c:344 ends
// every call with _out(f, "", 1), and a NUL would prefix the next ring line.
// s may be NULL with l 0 (fflush.c).
// __stdin_FILE stays weak: nothing reads stdin on the Saturn path, so a read
// stub would have no caller.
// `used`: at the yaul LTO link these hidden strong FILEs, referenced only from
// libyaul.a code linked outside LTO, were dropped, so libyaul's weak zero
// FILEs won; `used`
// keeps them emitted and global.
static size_t psyz_stdio_write(FILE* f, const unsigned char* s, size_t l) {
    size_t i;
    (void)f;
    for (i = 0; i < l; i++) {
        if (s[i]) {
            psyz_saturn_log[psyz_saturn_log_pos++ % LOGSZ] = s[i];
        }
    }
    return l;
}

__attribute__((used)) FILE __stdout_FILE = {.write = psyz_stdio_write};
__attribute__((used)) FILE __stderr_FILE = {.write = psyz_stdio_write};

static int pad_on;

// Pad frames from saturn_pad (SMPC INTBACK); padded like sdl3_common.h
// BuildPadFrame: 0xFF fill when disconnected (frame[1] == 0xFF), else 0.
void Psyz_PadsPoll(void) {
    int port;
    if (!pad_on) {
        saturn_pad_init(NULL);
        pad_on = 1;
    }
    for (port = 0; port < 2; port++) {
        char buf[PSYZ_PAD_BUF_LEN];
        const uint8_t* frame = saturn_pad_frame(port);
        memset(buf, frame[1] == 0xFF ? 0xFF : 0, sizeof buf);
        memcpy(buf, frame, 8);
        Psyz_PadsSet(port, buf, sizeof buf);
    }
}

int Psyz_VideoStats(PsyzVideoStats* stats) {
    (void)stats;
    return -1;
}

unsigned short* Psyz_VideoAllocCapturedVram(int* w, int* h) {
    (void)w;
    (void)h;
    return NULL;
}

int Psyz_VideoVSync(int mode) {
    static int frame;
    (void)mode;
    vdp2_sync();
    vdp2_sync_wait();
    if (pad_on) {
        saturn_pad_vsync();
    }
    return ++frame;
}

// VCNT holds a latched value: with EXLTEN=0 an EXTEN read latches it
// (ST-058-R2-060194 p0037, p0042). A plain read, not
// vdp2_tvmd_extern_latch (read-modify-write and an unbounded TVSTAT spin).
uint16_t saturn_frame_vcount(void) {
    (void)MEMORY_READ(16, VDP2(EXTEN));
    return vdp2_tvmd_vcount_get();
}

// saturn_cd_wait's pacing: one VCNT wrap per poll, so SATCD_WAIT_BOUND counts
// frames as PSY-Q's 0x4B0 VSyncs do. TVSTAT is unusable with the display off.
// Bound 200000 reads (design value).
void saturn_cd_frame_wait(void) {
    uint32_t n;
    uint16_t v, last = saturn_frame_vcount();
    for (n = 0; n < 200000; n++) {
        v = saturn_frame_vcount();
        if (v < last) {
            return;
        }
        last = v;
    }
}

// Saturn has no environment: every name is absent (defined C behaviour).
char* getenv(const char* name) {
    (void)name;
    return NULL;
}

// C11 7.22.1.4: defined behaviour; reached only when getenv is non-NULL
// (libgpu.c:202-210), never on Saturn.
unsigned long long strtoull(const char* s, char** end, int base) {
    const char* p = s;
    unsigned long long v = 0, lim;
    int neg = 0, any = 0, over = 0, d;
    if (base < 0 || base == 1 || base > 36) {
        if (end) {
            *end = (char*)s;
        }
        errno = EINVAL;
        return 0;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '+' || *p == '-') {
        neg = *p++ == '-';
    }
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    lim = ULLONG_MAX / (unsigned)base;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (isalpha((unsigned char)*p)) {
            d = tolower((unsigned char)*p) - 'a' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        any = 1;
        if (v > lim || v * base > ULLONG_MAX - (unsigned)d) {
            over = 1;
        }
        v = v * base + d;
    }
    if (end) {
        *end = (char*)(any ? p : s);
    }
    if (over) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? -v : v;
}

void user_init(void) {} // libyaul crt0 hook
