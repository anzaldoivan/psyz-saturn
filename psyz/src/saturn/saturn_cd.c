/* saturn_cd.c -- Saturn CD owner (see saturn_cd.h). */
#include "saturn_cd.h"

#include <stddef.h>

struct saturn_cd_stats saturn_cd_stats;

static uint32_t track_fad[SATCD_MAX_TRACKS];
static int track_count;
static uint32_t leadout;

static struct {
    int state; /* SATCD_* */
    int kind;  /* 0 none, 1 read, 2 seek */
    uint32_t n, done, age;
    uint8_t* dst;
} op;
static int play_open;

int saturn_cd_init(void) {
    int n = saturn_cdhw_toc(track_fad, SATCD_MAX_TRACKS, &leadout);
    track_count = n > 0 ? n : 0;
    op.kind = 0;
    op.state = SATCD_IDLE;
    return track_count;
}

int saturn_cd_tracks(uint32_t* fad, int max) {
    int i;
    for (i = 0; i < track_count && i < max; i++)
        fad[i] = track_fad[i];
    return i;
}

uint32_t saturn_cd_leadout(void) { return track_count ? leadout : 0; }

int saturn_cd_fad_valid(uint32_t fad, uint32_t n) {
    return track_count > 0 && n > 0 && fad >= track_fad[0] && fad < leadout &&
           n <= leadout - fad;
}

/* The only place the pregap bias is applied. */
static uint32_t bias(void) { return SATCD_FAD_BIAS; }

uint32_t saturn_cd_lba_to_fad(int32_t lba) {
    return (uint32_t)(lba + (int32_t)bias());
}
int32_t saturn_cd_fad_to_lba(uint32_t fad) {
    return (int32_t)fad - (int32_t)bias();
}

int saturn_cd_busy(void) { return op.kind != 0; }
uint32_t saturn_cd_remaining(void) { return op.kind == 1 ? op.n - op.done : 0; }
uint32_t saturn_cd_done_sectors(void) { return op.kind == 1 ? op.done : 0; }

static int start(int kind, uint32_t fad, uint32_t n, void* dst) {
    if (op.kind != 0 || !saturn_cd_fad_valid(fad, n)) {
        saturn_cd_stats.rejects++;
        return 0;
    }
    if ((kind == 1 ? saturn_cdhw_read(fad, n) : saturn_cdhw_seek(fad)) != 0) {
        saturn_cd_stats.rejects++;
        return 0;
    }
    op.kind = kind;
    op.n = n;
    op.done = op.age = 0;
    op.dst = dst;
    op.state = SATCD_BUSY;
    return 1;
}

int saturn_cd_start_read(uint32_t fad, uint32_t n, void* dst) {
    return start(1, fad, n, dst);
}

int saturn_cd_start_seek(uint32_t fad) { return start(2, fad, 1, NULL); }

int saturn_cd_play_open(uint32_t fad, uint32_t n) {
    if (op.kind != 0 || !saturn_cd_fad_valid(fad, n) ||
        saturn_cdhw_play(fad, n) != 0) {
        saturn_cd_stats.rejects++;
        return 0;
    }
    play_open = 1;
    return 1;
}

static void finish(int state) {
    op.kind = 0;
    op.state = state;
}

void saturn_cd_abort(void) {
    if (op.kind != 0 || play_open)
        saturn_cdhw_abort();
    play_open = 0;
    finish(SATCD_IDLE);
}

int saturn_cd_poll(void) {
    int r;
    if (op.kind == 0)
        return op.state;
    if (++op.age > SATCD_WAIT_BOUND) { /* age bounds mode-1 polls too */
        saturn_cdhw_abort();
        saturn_cd_stats.timeouts++;
        finish(SATCD_ERROR);
        return op.state;
    }
    r = saturn_cdhw_poll();
    if (r == SATCD_HW_SECTOR && op.kind == 1) {
        op.done += saturn_cdhw_take(op.dst + (size_t)op.done * SATCD_SECTOR);
        if (op.done == op.n)
            finish(SATCD_DONE);
    } else if (r == SATCD_HW_DONE && op.kind == 2) {
        finish(SATCD_DONE);
    } else if (
        r == SATCD_HW_ERROR || r == SATCD_HW_SECTOR || r == SATCD_HW_DONE) {
        saturn_cdhw_abort(); /* error, or a report that does not fit the op */
        saturn_cd_stats.errors++;
        finish(SATCD_ERROR);
    }
    return op.state;
}

/* one frame between polls; the platform glue (saturn_misc.c) overrides it */
__attribute__((weak)) void saturn_cd_frame_wait(void) {}

int saturn_cd_wait(void) {
    uint32_t i = 0;
    while (op.kind != 0) {
        saturn_cd_poll();
        saturn_cd_frame_wait();
        if (++i >= SATCD_WAIT_BOUND && op.kind != 0) {
            saturn_cdhw_abort();
            saturn_cd_stats.timeouts++;
            finish(SATCD_ERROR);
        }
    }
    return op.state;
}

int saturn_cd_read_sync(uint32_t fad, uint32_t n, void* dst) {
    return saturn_cd_start_read(fad, n, dst) && saturn_cd_wait() == SATCD_DONE;
}
