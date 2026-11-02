/* saturn_movie.c -- movie service (saturn_movie.h). Implements the decoder
 * seams of saturn_movie_decoder.h:
 * - CD: the file is read only through the owner (saturn_cd), one op in flight,
 *   into a ring of 4 units x 16 sectors; the next read starts when a unit is
 *   free and sectors remain; bytes landed in the in-flight unit may be read.
 *   One play covers the whole file (saturn_cd_play_open), so the unit reads
 *   continue it without a seek; refused, each unit read plays its own range.
 *   At most MOVIE_POLLS owner polls per counted VBlank (the VSync pump adds
 *   one), stopping on a busy port, so the owner's age bound stays frame-paced.
 * - Clock: movie_tick reads the display seam's VBlank-in count
 *   (saturn_gpu_movie_vblanks), kept by an interrupt, so a decode longer than
 *   a frame loses no VBlank (a VCNT wrap count did). Frame k is due at v0 + 4k
 *   (v0: frame 0's VBlank), enqueued at the first point the count reaches it;
 *   its DMA runs at the next VBlank-in, which shows it. A late frame is shown
 *   when ready; one a frame period (4 VBlanks) or more past due is dropped, not
 *   enqueued, so a slow stretch costs frames instead of pushing later frames
 *   off the schedule. The next due is unchanged either way. A frame enqueued
 *   while the previous DMA is still queued waits one VBlank, once.
 * - Log (saturn_movie_log_set): entry k written when frame k is enqueued or
 *   dropped (k with bit 31 set), only for k < n and a non-NULL log.
 * - Every wait is bounded; every end path aborts the owner's op and ends the
 *   sound and display seams. Platform-free: no libyaul header. */
#include "saturn_movie.h"
#include "saturn_cd.h"
#include "saturn_movie_decoder.h"

#include <libcd.h>
#include <psyz/video.h>
#include <string.h>

#define MOVIE_UNIT_SECTORS 16
#define MOVIE_UNIT (MOVIE_UNIT_SECTORS * SATCD_SECTOR)
#define MOVIE_UNITS 5
#define MOVIE_RING (MOVIE_UNITS * MOVIE_UNIT)
#define MOVIE_POLLS 4
#define MOVIE_UNDERRUN_FRAMES 120
#define MOVIE_VBLANKS_PER_FRAME 4
#define MOVIE_SPIN_BOUND 200000 /* clock reads per wait, as saturn_misc */
#define MOVIE_RATE 22050
#define MOVIE_TAIL 0x1000 /* silence after the last PCM: one SPU CA block */
#define MOVIE_DRAIN_MARGIN 4
#define MOVIE_CACHE 2048
/* mono sound written this many bytes ahead of the decoder's audio */
#define MOVIE_SOUND_AHEAD 0x2000

/* shared with saturn_music.c: the two never run at once */
__attribute__((weak)) uint8_t saturn_stream_ring[MOVIE_RING];
static film_sample_t cache[MOVIE_CACHE];
static saturn_movie_stats_t stats;
static struct saturn_cd_stats cd0;

static uint32_t fad0, size, nsec, nunits, issued, landed, rejected;
static int inflight, sound;
static uint32_t pos; /* the decoder's file offset */
static uint32_t vbl, v0, budget;
static saturn_movie_frame_t* log_buf;
static uint32_t log_n, issue_v, lat_max;
static saturn_movie_frame_t pend; /* shown entry until its VBlank-in counts */
static int pend_on;
/* sound look-ahead: table walk (sample a_i, file a_off..a_end), bytes the
 * decoder consumed, bytes written */
static uint32_t a_n, a_i, a_off, a_end, a_used, a_done;
static int ahead;

static void fail(int s) {
    if (stats.status == SATURN_MOVIE_END)
        stats.status = s;
}

static uint32_t unit_sectors(uint32_t j) {
    uint32_t s = j * MOVIE_UNIT_SECTORS;
    return nsec - s < MOVIE_UNIT_SECTORS ? nsec - s : MOVIE_UNIT_SECTORS;
}

/* File bytes in the ring: whole units, then the in-flight unit's sectors. */
static uint32_t landed_end(void) {
    uint32_t e = landed * MOVIE_UNIT;
    if (inflight)
        e += saturn_cd_done_sectors() * SATCD_SECTOR;
    return e < size ? e : size;
}

/* Settle a finished op, start the next unit, poll within the VBlank's budget.
 */
static void cd_step(void) {
    uint32_t d;
    for (;;) {
        if (stats.status != SATURN_MOVIE_END)
            return;
        if (inflight && !saturn_cd_busy()) {
            inflight = 0;
            if (saturn_cd_poll() == SATCD_ERROR) { /* idle: the op's end */
                fail(saturn_cd_stats.timeouts != cd0.timeouts
                         ? SATURN_MOVIE_CD_TIMEOUT
                         : SATURN_MOVIE_CD_ERROR);
                return;
            }
            stats.bytes_read += unit_sectors(landed) * SATCD_SECTOR;
            if (vbl - issue_v > lat_max)
                lat_max = vbl - issue_v;
            landed++;
        }
        if (!inflight && issued < nunits &&
            issued < pos / MOVIE_UNIT + MOVIE_UNITS) {
            if (!saturn_cd_start_read(
                    fad0 + issued * MOVIE_UNIT_SECTORS, unit_sectors(issued),
                    saturn_stream_ring + (issued % MOVIE_UNITS) * MOVIE_UNIT)) {
                if (++rejected >= 2)
                    fail(SATURN_MOVIE_CD_REJECT);
                return;
            }
            rejected = 0;
            inflight = 1;
            issue_v = vbl;
            issued++;
        }
        if (!inflight || budget == 0)
            return;
        d = saturn_cd_done_sectors();
        saturn_cd_poll();
        budget--;
        stats.polls++;
        if (saturn_cd_busy() && saturn_cd_done_sectors() == d) {
            budget = 0; /* port busy: no more polls this VBlank */
            return;
        }
    }
}

void movie_tick(void) {
    uint32_t v = saturn_gpu_movie_vblanks();
    if (v != vbl) {
        vbl = v;
        budget = MOVIE_POLLS;
    }
    if (pend_on && vbl >= pend.shown) {
        if (log_buf && pend.k < log_n)
            log_buf[pend.k] = pend;
        pend_on = 0;
    }
    cd_step();
}

/* One VSync (the pumps run), then ticks until the next VBlank-in is counted. */
static void wait_vblank(void) {
    uint32_t n, b = vbl;
    Psyz_VideoVSync(0);
    for (n = 0; vbl == b && n < MOVIE_SPIN_BOUND; n++)
        movie_tick();
}

/* Natural end: a silent tail after the last PCM, then VBlanks until the
 * stream's play block reaches it, bounded by the PCM still ahead of it. No CD
 * op: the owner is idle, and the waits do not tick the service. */
static void drain(void) {
    static const uint8_t silence[256];
    uint32_t i, n, b, bound;
    int lead;
    for (i = 0; i < MOVIE_TAIL; i += sizeof silence)
        saturn_spu_movie_write(silence, sizeof silence);
    lead = saturn_spu_movie_lead();
    if (lead <= MOVIE_TAIL)
        return;
    bound =
        (uint32_t)(lead - MOVIE_TAIL) * 60 / MOVIE_RATE + MOVIE_DRAIN_MARGIN;
    for (i = 0; i < bound && saturn_spu_movie_lead() > MOVIE_TAIL; i++) {
        b = saturn_gpu_movie_vblanks();
        Psyz_VideoVSync(0);
        for (n = 0; n < MOVIE_SPIN_BOUND; n++)
            if (saturn_gpu_movie_vblanks() != b)
                break;
    }
}

static int prefill(void) {
    uint32_t w, full = nunits < MOVIE_UNITS ? nunits : MOVIE_UNITS;
    for (w = 0; landed < full && stats.status == SATURN_MOVIE_END; w++) {
        if (w == MOVIE_UNDERRUN_FRAMES) {
            fail(SATURN_MOVIE_UNDERRUN);
            break;
        }
        wait_vblank();
    }
    return stats.status == SATURN_MOVIE_END;
}

/* Waits VBlanks until file bytes up to end have landed: 120 in a row without
 * progress -> UNDERRUN. Nonzero = stop. */
static int wait_landed(uint32_t end) {
    uint32_t w, e;
    for (w = 0, e = landed_end(); end > e; e = landed_end()) {
        if (stats.status != SATURN_MOVIE_END)
            return 1;
        if (w == MOVIE_UNDERRUN_FRAMES) {
            fail(SATURN_MOVIE_UNDERRUN);
            return 1;
        }
        wait_vblank();
        stats.underrun_frames++;
        w = landed_end() > e ? 0 : w + 1;
    }
    return 0;
}

/* Writes mono sound until MOVIE_SOUND_AHEAD bytes ahead of the decoder's
 * audio: the table's audio samples from landed bytes inside pos's ring pass
 * (units the CD may not reissue), silence after the table's end. */
static void pump(void) {
    static const uint8_t silence[256];
    uint32_t k, e = pos / MOVIE_UNIT * MOVIE_UNIT + MOVIE_RING;
    if (e > landed_end())
        e = landed_end();
    while (a_done < a_used + MOVIE_SOUND_AHEAD) {
        if (a_i >= a_n) {
            k = a_used + MOVIE_SOUND_AHEAD - a_done;
            if (k > sizeof silence)
                k = sizeof silence;
            saturn_spu_movie_write(silence, k);
            a_done += k;
        } else if (a_off == a_end) {
            if (++a_i < a_n)
                a_end += cache[a_i].length;
        } else if (cache[a_i].interval != 0xFFFFFFFF) {
            a_off = a_end;
        } else {
            if (a_off >= e)
                return;
            k = MOVIE_UNIT - a_off % MOVIE_UNIT;
            if (k > a_end - a_off)
                k = a_end - a_off;
            if (k > e - a_off)
                k = e - a_off;
            saturn_spu_movie_write(saturn_stream_ring + a_off % MOVIE_RING, k);
            a_off += k;
            a_done += k;
        }
    }
}

/* The next n file bytes, one unit piece at a time (a piece never wraps the
 * ring): into dst, skipped (dst NULL), or to the sound seam (audio; mono
 * through the look-ahead pump). Past the
 * file end dst gets zeros. Waits VBlanks for bytes not landed: 120 in a row
 * without progress -> UNDERRUN. Nonzero = stop. */
static int take(uint8_t* dst, uint32_t n, int audio) {
    uint32_t k;
    while (n > 0 && stats.status == SATURN_MOVIE_END) {
        k = MOVIE_UNIT - pos % MOVIE_UNIT;
        if (k > n)
            k = n;
        if (pos >= size) {
            if (dst)
                memset(dst, 0, k);
        } else {
            if (k > size - pos)
                k = size - pos;
            if (wait_landed(pos + k))
                return 1;
            if (dst)
                memcpy(dst, saturn_stream_ring + pos % MOVIE_RING, k);
            else if (audio && sound && ahead) {
                a_used += k;
                pump();
            } else if (audio && sound)
                saturn_spu_movie_write(
                    saturn_stream_ring + pos % MOVIE_RING, k);
        }
        pos += k;
        n -= k;
        if (dst)
            dst += k;
    }
    return stats.status != SATURN_MOVIE_END;
}

int movie_stream_refill(void* dst, uint32_t n) {
    return take((uint8_t*)dst, n, 0);
}

/* The next n file bytes in place: pos..pos+n must lie inside the file and
 * inside one pass of the ring (no wrap), else NULL with pos unchanged. The
 * pointer stays valid until the decoder's next stream call: the CD writes only
 * units before pos's unit, and only from movie_tick, which the decoder calls at
 * strip and chunk boundaries, after it has consumed the bytes. */
const uint8_t* movie_stream_view(uint32_t n) {
    const uint8_t* p;
    if (n == 0 || pos >= size || n > size - pos ||
        pos % MOVIE_RING + n > MOVIE_RING || stats.status != SATURN_MOVIE_END ||
        wait_landed(pos + n))
        return NULL;
    p = saturn_stream_ring + pos % MOVIE_RING;
    pos += n;
    return p;
}

int movie_audio_chunk(uint32_t n) { return take(NULL, n, 1); }

void movie_audio_table(uint32_t n, uint32_t channels) {
    a_n = n;
    a_i = 0;
    a_off = pos;
    a_end = pos + cache[0].length;
    ahead = sound && channels == 1;
}

static void log_frame(uint32_t k, uint32_t due, uint32_t shown, uint32_t lead) {
    saturn_movie_frame_t* f;
    if (!log_buf || (k & 0x7FFFFFFFu) >= log_n)
        return;
    if (k & 0x80000000u) {
        f = &log_buf[k & 0x7FFFFFFFu];
    } else {
        if (pend_on)
            log_buf[pend.k] = pend;
        f = &pend;
        pend_on = 1;
    }
    f->k = k;
    f->due = due;
    f->shown = shown;
    f->sectors = (landed_end() + SATCD_SECTOR - 1) / SATCD_SECTOR;
    f->lead = lead;
    f->latency = lat_max;
    f->polls = stats.polls;
    f->underrun_frames = stats.underrun_frames;
    lat_max = 0;
}

int movie_frame_out(const uint16_t* rgb555, uint32_t k) {
    uint32_t n, due, e, lead;
    if (k == 0)
        v0 = vbl;
    due = v0 + MOVIE_VBLANKS_PER_FRAME * k;
    for (n = 0; vbl < due && n < MOVIE_VBLANKS_PER_FRAME &&
                stats.status == SATURN_MOVIE_END;
         n++)
        wait_vblank();
    if (stats.status != SATURN_MOVIE_END)
        return 1;
    e = landed_end();
    lead = e > pos ? e - pos : 0;
    if (vbl >= due + MOVIE_VBLANKS_PER_FRAME) {
        stats.dropped++;
        log_frame(k | 0x80000000u, due, vbl, lead);
        return 0;
    }
    if (vbl > due) {
        stats.late_frames++;
        if (vbl - due > stats.max_late)
            stats.max_late = vbl - due;
    }
    if (saturn_gpu_movie_frame(rgb555) != 0) {
        wait_vblank(); /* the previous frame's DMA runs at this VBlank-in */
        if (saturn_gpu_movie_frame(rgb555) != 0) {
            fail(SATURN_MOVIE_DMA_PENDING);
            return 1;
        }
    }
    stats.frames++;
    log_frame(k, due, vbl + 1, lead);
    return stats.status != SATURN_MOVIE_END;
}

int film_loop_handler(void) { return 1; }

void saturn_movie_log_set(saturn_movie_frame_t* log, uint32_t n) {
    log_buf = log;
    log_n = log ? n : 0;
}

int saturn_movie_play(const char* iso_path, saturn_movie_stats_t* out) {
    CdlFILE f;
    int dec = -1;
    memset(&stats, 0, sizeof stats);
    cd0 = saturn_cd_stats;
    issued = landed = rejected = pos = vbl = v0 = issue_v = lat_max = 0;
    a_n = a_i = a_off = a_end = a_used = a_done = 0;
    inflight = sound = ahead = pend_on = 0;
    budget = MOVIE_POLLS;
    if (!CdSearchFile(&f, (char*)iso_path)) {
        fail(SATURN_MOVIE_NOT_FOUND);
    } else if (saturn_gpu_movie_begin() < 0) {
        fail(SATURN_MOVIE_DISPLAY_REFUSED);
    } else {
        fad0 = saturn_cd_lba_to_fad(CdPosToInt(&f.pos));
        size = (uint32_t)f.size;
        nsec = (size + SATCD_SECTOR - 1) / SATCD_SECTOR;
        nunits = (nsec + MOVIE_UNIT_SECTORS - 1) / MOVIE_UNIT_SECTORS;
        (void)saturn_cd_play_open(fad0, nsec);
        sound = saturn_spu_movie_begin(MOVIE_RATE) >= 0;
        stats.sound_refused = !sound;
        vbl = saturn_gpu_movie_vblanks();
        cd_step();
        if (prefill()) {
            dec = movie_decoder_play(size, cache, MOVIE_CACHE);
            if (dec == MOVIE_CHUNK_SHORT || dec == MOVIE_STRIP_SHORT)
                fail(SATURN_MOVIE_GUARD);
            if (dec == MOVIE_BAD_HEADER)
                fail(SATURN_MOVIE_BAD_HEADER);
            if (stats.status == SATURN_MOVIE_END)
                wait_vblank(); /* the last frame's DMA, before the plane ends */
        }
        saturn_cd_abort();
        if (sound && stats.status == SATURN_MOVIE_END && dec == MOVIE_END)
            drain();
        if (sound)
            saturn_spu_movie_end();
        saturn_gpu_movie_end();
    }
    stats.cd_rejects = saturn_cd_stats.rejects - cd0.rejects;
    stats.cd_timeouts = saturn_cd_stats.timeouts - cd0.timeouts;
    stats.cd_errors = saturn_cd_stats.errors - cd0.errors;
    if (out)
        *out = stats;
    return stats.status;
}
