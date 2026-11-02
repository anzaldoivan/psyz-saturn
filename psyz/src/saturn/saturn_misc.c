#include <yaul.h>
#include <psyz.h>
#include <psyz/input.h>
#include <psyz/log.h>
#include <psyz/video.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "saturn_pad.h"

// psyz/system.h renames write, which is also a member of libyaul's FILE
#undef write

#define LOGSZ 4096

LOG_LEVEL psyz_logLevel = LOG_LEVEL_I;

// log ring: the Saturn title has no serial output
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

// libyaul's weak zero stdout/stderr FILEs make printf jump to 0; `used` keeps
// these strong ones past the LTO link
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
uint16_t saturn_frame_vcount(void) {
    (void)MEMORY_READ(16, VDP2(EXTEN));
    return vdp2_tvmd_vcount_get();
}

void user_init(void) {}
