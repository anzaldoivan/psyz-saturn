// Saturn host primitives for kernel.c: one thread, no scheduler. The kernel
// runs in polled mode (Psyz_OsThreadStart fails); a bell wait spins on the
// clock until it is rung or the timeout passes.
#include <stddef.h>
#include <yaul.h>
#include "../psyz/os.h"

// FRT counts at phi/8 (cpu_frt_init(CPU_FRT_CLOCK_DIV_8) in libyaul's
// cpu_init); phi follows the dot clock (BIOS clock change on 320/352 modes).
static uint32_t FrtCountPerMs(void) {
    int hi = bios_clock_speed_get() == CPU_CLOCK_SPEED_28MHZ;
    if (vdp2_tvmd_tv_standard_get() == VDP2_TVMD_TV_STANDARD_PAL) {
        return hi ? CPU_FRT_PAL_352_8_COUNT_1MS : CPU_FRT_PAL_320_8_COUNT_1MS;
    }
    return hi ? CPU_FRT_NTSC_352_8_COUNT_1MS : CPU_FRT_NTSC_320_8_COUNT_1MS;
}

// Extended past 16 bits from deltas: a gap between two reads longer than one
// FRT wrap (about 19 ms at phi/8) is lost.
static uint32_t now_us, frt_frac;
static uint16_t frt_last;

uint32_t Psyz_OsNowUs(void) {
    uint32_t per_ms = FrtCountPerMs();
    uint16_t c = cpu_frt_count_get();
    frt_frac += (uint32_t)(uint16_t)(c - frt_last) * 1000u;
    frt_last = c;
    now_us += frt_frac / per_ms;
    frt_frac %= per_ms;
    return now_us;
}

uintptr_t Psyz_OsThreadSelf(void) { return 1; }

#define POOL 4

static int locks[POOL];
static volatile int bells[POOL];
static int nlocks, nbells;

void* Psyz_OsLockCreate(void) {
    return nlocks < POOL ? &locks[nlocks++] : NULL;
}

void Psyz_OsLock(void* lock) { *(int*)lock = 1; }
void Psyz_OsUnlock(void* lock) { *(int*)lock = 0; }

void* Psyz_OsBellCreate(void) {
    return nbells < POOL ? (void*)&bells[nbells++] : NULL;
}

void Psyz_OsBellRing(void* bell) { *(volatile int*)bell = 1; }

void Psyz_OsBellWait(void* bell, uint32_t timeout_us) {
    volatile int* b = bell;
    uint32_t start = Psyz_OsNowUs();
    while (!*b && (timeout_us == PSYZ_OS_FOREVER ||
                   Psyz_OsNowUs() - start < timeout_us)) {
    }
    *b = 0;
}

int Psyz_OsThreadStart(void (*fn)(void), const char* name) {
    (void)fn;
    (void)name;
    return -1;
}
