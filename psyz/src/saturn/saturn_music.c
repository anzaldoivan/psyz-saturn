/* saturn_music.c -- music stream (saturn_music.h).
 * - Lookup: the record whose [xa_lba, xa_lba + xa_sectors) holds the FAD's
 *   LBA and, with CdlModeSF, whose chan is the filter's; without SF the first
 *   such record. Offset: ((lba - xa_lba - phase) / stride) x 1176 frames (one
 *   XA sector's 2,016 frames at 37,800 Hz, at 22,050 Hz); at or past frames
 *   the hit plays silence.
 * - CD: one unit (16 sectors) per op through the owner, into the main-RAM
 *   ring of 5 units shared with the movie (the two never run at once); the
 *   next unit starts when the owner is idle and less than MUSIC_HIGH units
 *   are landed and unwritten.
 * - Sound: the seam begins at the first landed unit; each step tops the lead
 *   up in pieces of at most one block per channel while lead + piece stays
 *   within LEAD_MAX, so the seam never waits. A restart (ReadS / ReadN while
 *   on) aborts only the stream's op, drops the landed units and writes
 *   silence until the new first unit lands: no key-off, no re-key. Past
 *   frames, silence. */
#include "saturn_music.h"
#include "saturn_cd.h"
#include "saturn_spu.h"

#include <string.h>

#define MUSIC_UNIT_SECTORS 16
#define MUSIC_UNIT (MUSIC_UNIT_SECTORS * SATCD_SECTOR)
#define MUSIC_HALF (MUSIC_UNIT / 2) /* bytes per channel per unit */
#define MUSIC_UNITS 5
#define MUSIC_HIGH 3
#define MUSIC_FRAMES_PER_SECTOR 1176
#define MUSIC_BLK 0x1000  /* sound ring block, samples */
#define MUSIC_RING 0x4000 /* sound ring per channel, samples */

enum { MUSIC_OFF, MUSIC_STARTING, MUSIC_PLAYING, MUSIC_RESTARTING };

__attribute__((weak)) uint8_t saturn_stream_ring[MUSIC_UNITS * MUSIC_UNIT];

static const uint8_t zero[0x400];
static saturn_music_stats_t stats;
static const saturn_music_rec_t* rec;
static int state, inflight, primed;
/* per-channel bytes: write position, end (frames), first unit, units landed
 * since the (re)start, bytes written since begin */
static uint32_t pos, end, u0, landed, nunits, wr;
static uint32_t blk, lead_min, lead_max;

const saturn_music_stats_t* saturn_music_stats(void) { return &stats; }

int saturn_music_on(void) { return state == MUSIC_OFF ? 0 : inflight ? 2 : 1; }

void saturn_music_slice(void) { stats.slices++; }

static const saturn_music_rec_t* lookup(uint32_t fad, int chan) {
    int32_t lba = saturn_cd_fad_to_lba(fad);
    int i;
    for (i = 0; i < saturn_music_map_n; i++) {
        const saturn_music_rec_t* r = &saturn_music_map[i];
        if (lba >= 0 && (uint32_t)lba - r->xa_lba < r->xa_sectors &&
            (chan < 0 || r->chan == (uint32_t)chan))
            return r;
    }
    return 0;
}

int saturn_music_read(uint32_t fad, int chan) {
    const saturn_music_rec_t* r = lookup(fad, chan);
    uint32_t d, bps;
    if (!r)
        return 0;
    if (state == MUSIC_OFF) {
        memset(&stats, 0, sizeof stats);
        stats.cover_min = 0xFFFFFFFFu;
        state = MUSIC_STARTING;
    } else {
        if (inflight)
            saturn_cd_abort();
        inflight = 0;
        if (state != MUSIC_STARTING) {
            state = MUSIC_RESTARTING;
            stats.restarts++;
        }
    }
    stats.nofilter += chan < 0;
    rec = r;
    bps = r->format ? 2 : 1;
    d = (uint32_t)saturn_cd_fad_to_lba(fad) - r->xa_lba;
    d = d > r->phase ? d - r->phase : 0;
    pos = d / r->stride * MUSIC_FRAMES_PER_SECTOR * bps;
    end = r->frames * bps;
    nunits = (end + MUSIC_HALF - 1) / MUSIC_HALF;
    u0 = pos / MUSIC_HALF;
    landed = 0;
    primed = 0;
    blk = MUSIC_BLK * bps;
    lead_min = 2 * blk;
    lead_max = MUSIC_RING * bps - blk;
    return 1;
}

void saturn_music_end(void) {
    if (inflight)
        saturn_cd_abort();
    inflight = 0;
    if (state == MUSIC_PLAYING || state == MUSIC_RESTARTING)
        saturn_spu_stream_end();
    state = MUSIC_OFF;
}

static void cd_step(void) {
    uint32_t u = u0 + landed;
    if (inflight && !saturn_cd_busy()) {
        inflight = 0;
        if (saturn_cd_poll() == SATCD_DONE) { /* idle: the op's end */
            landed++;
            stats.units++;
        } /* else the unit is read again */
        u = u0 + landed;
    }
    primed |= u * MUSIC_HALF >= pos + MUSIC_HIGH * MUSIC_HALF;
    /* below MUSIC_HIGH the unit's slot is free: u < pos / MUSIC_HALF + 4 */
    if (!inflight && !saturn_cd_busy() && u < nunits &&
        u * MUSIC_HALF < pos + MUSIC_HIGH * MUSIC_HALF &&
        saturn_cd_start_read(
            rec->pcm_fad + u * MUSIC_UNIT_SECTORS, MUSIC_UNIT_SECTORS,
            saturn_stream_ring + u % MUSIC_UNITS * MUSIC_UNIT))
        inflight = 1;
}

void saturn_music_step(void) {
    uint32_t lend, lead, n;
    const uint8_t* l;
    int k;
    if (state == MUSIC_OFF)
        return;
    cd_step();
    /* a start waits for a full lead's bytes (two units of PCM16), a restart
     * for its first unit */
    if (state != MUSIC_PLAYING &&
        ((landed && (u0 + landed) * MUSIC_HALF - pos >=
                        (state == MUSIC_STARTING ? lead_max : 1)) ||
         u0 + landed >= nunits)) {
        if (state == MUSIC_STARTING) {
            saturn_spu_stream_begin(rec->rate, 2, rec->format ? 16 : 8);
            wr = 0;
        }
        state = MUSIC_PLAYING;
    }
    if (state == MUSIC_STARTING)
        return;
    lend = (u0 + landed) * MUSIC_HALF;
    lend = lend < end ? lend : end;
    for (;;) {
        k = saturn_spu_stream_lead();
        lead = k < 0 ? wr : (uint32_t)k; /* no slot yet: bytes since begin */
        if (lead > lead_max)
            lead = 0; /* wrapped: behind the head, the write resyncs */
        if (lead == lead_max)
            break;
        n = lead_max - lead < blk ? lead_max - lead : blk;
        if (state == MUSIC_RESTARTING || pos >= end) {
            n = n < sizeof zero ? n : sizeof zero;
            saturn_spu_stream_write(zero, zero, n);
        } else {
            if (lend - pos < n)
                n = lend - pos;
            if (MUSIC_HALF - pos % MUSIC_HALF < n)
                n = MUSIC_HALF - pos % MUSIC_HALF;
            if (n == 0) {
                if (lead < lead_min)
                    stats.underrun++;
                break;
            }
            l = saturn_stream_ring +
                pos / MUSIC_HALF % MUSIC_UNITS * MUSIC_UNIT + pos % MUSIC_HALF;
            saturn_spu_stream_write(l, l + MUSIC_HALF, n);
            pos += n;
        }
        wr += n;
    }
    if (state == MUSIC_PLAYING && primed && u0 + landed < nunits) {
        n = (u0 + landed) * MUSIC_HALF;
        n = (n > pos ? n - pos : 0) + lead;
        if (n < stats.cover_min)
            stats.cover_min = n;
    }
}
