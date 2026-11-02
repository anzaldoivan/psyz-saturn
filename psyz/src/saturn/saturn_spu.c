/* saturn_spu.c -- PS1 SPU seam on the Saturn SCSP through the ponèSound 68000
 * driver. Every register
 * write lands in a 0x200-byte shadow (reads come from it, with the desktop's
 * trans_addr and spustat models, psyz_spu.c:360-376); voice v (0-23) maps onto
 * the driver's command entry v (_PCM_CTRL, 20 B). Idle until the first of the
 * five seam calls, which loads the driver once: SNDOFF, common register,
 * clear 0x0..0xC028, copy, SNDON (ST-166 p0022; TB51 p0002-p0003); never a
 * second pair. saturn_spu_pump() (Psyz_VideoVSync) waits for the driver's
 * ready value, then per frame writes the dirty entries and start = 1: every
 * sound-RAM write except the load, Psyz_SpuMemWrite and the stream's rings
 * (saturn_spu_stream_*, entries NV and NV + 1) happens
 * there. SATURN_SPU_HOST turns sound RAM and the common register into fixture
 * symbols; multi-byte fields are then composed big-endian.
 *
 * The _PCM_CTRL field layout and the sysComPara offsets come from the
 * ponèSound driver interface (pcmsys.h:110-136); no driver code is used:
 *
 * Copyright (c) 2020 ponut64
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "saturn_spu.h"
#include "saturn_movie.h"
#include "saturn_pad.h"
#include <psyz/spu.h>
#include <string.h>

#define RAM_BASE 0x25A00000u /* sound RAM, cache-through view */
#define COMMON 0x25B00400u   /* MEM4MB | MVOL (ST-077 p0101) */
#define MONITOR 0x25B00408u  /* MSLC write bits 11-15, CA read bits 7-10 */
#define DRV_LO 0x408u        /* driver program start (pcmsys.h:51) */
#define SYS 0xC008u          /* sysComPara: 0x408 + 47 KiB */
#define SYS_START (SYS + 4)  /* u16 start */
#define SYS_CTRL (SYS + 12)  /* u32 pcmCtrl SH-2 address */
#define CLEAR_END 0xC028u    /* driver footprint; samples from here */
#define READY 0x7777u        /* drv main.c:414 */
#define ENTRY 20u            /* sizeof(_PCM_CTRL) */
#define ENTRIES 96u          /* PCM_CTRL_MAX 93 + ADX_CTRL_MAX 3 */
#define REGION_LO 0x1010u    /* first SPU sample byte (SpuMalloc) */
#define REGION_END 0x6BFE8u  /* SPU end below MOVIE_LO */
#define SPU_TO_RAM 0xB018u   /* 0xC028 - 0x1010 */
#define ADDR_MAX 0x7F000u    /* PCMEND */
#define MOVIE_LO 0x77000u    /* PCM8 stream span, top of the PCM area */
#define NV 24                /* also the stream's L (or mono) entry index */
#define STREAM_SPAN 0x8000u  /* span below ADDR_MAX per byte of a sample */
#define STREAM_BLK 0x1000u   /* CA granularity, samples: address bits 15..12 */
#define STREAM_WAIT_BOUND 200000 /* play position reads per write */

/* SPU_RXX offsets (decomp libspu_private.h:84-147) */
#define R_MAIN_L 0x180u
#define R_MAIN_R 0x182u
#define R_CD_VOL 0x1B0u
#define R_KEY_ON 0x188u
#define R_KEY_OFF 0x18Cu
#define R_TRANS_ADDR 0x1A6u
#define R_SPUCNT 0x1AAu
#define R_SPUSTAT 0x1AEu
#define CNT_MAPPED 0xC000u /* spucnt enable | mute */
#define SH(off) s.sh[(off) >> 1]
#define VREG(v, r) SH((unsigned)(v) * 0x10u + (r))

#ifdef SATURN_SPU_HOST
extern uint8_t saturn_spu_hw_ram[0x80000];
void saturn_spu_hw_common(uint16_t v);
uint16_t saturn_spu_hw_monitor(uint16_t w);
#define RAM8(o) saturn_spu_hw_ram[(o)]
#define RAM16(o) ((uint16_t)(RAM8(o) << 8 | RAM8((o) + 1)))
#define RAM32(o) ((uint32_t)RAM16(o) << 16 | RAM16((o) + 2))
static void set16(uint32_t o, uint32_t v) {
    RAM8(o) = (uint8_t)(v >> 8);
    RAM8(o + 1) = (uint8_t)v;
}
static void set32(uint32_t o, uint32_t v) {
    set16(o, v >> 16);
    set16(o + 2, v);
}
#define SET8(o, v) (RAM8(o) = (uint8_t)(v))
#define SET16(o, v) set16((o), (v))
#define SET32(o, v) set32((o), (v))
#define SET_COMMON(v) saturn_spu_hw_common((uint16_t)(v))
#define READ_MONITOR(w) saturn_spu_hw_monitor((uint16_t)(w))
#else
#define RAM8(o) (*(volatile uint8_t*)(uintptr_t)(RAM_BASE + (o)))
#define RAM16(o) (*(volatile uint16_t*)(uintptr_t)(RAM_BASE + (o)))
#define RAM32(o) (*(volatile uint32_t*)(uintptr_t)(RAM_BASE + (o)))
#define SET8(o, v) (RAM8(o) = (uint8_t)(v))
#define SET16(o, v) (RAM16(o) = (uint16_t)(v))
#define SET32(o, v) (RAM32(o) = (uint32_t)(v))
#define SET_COMMON(v) (*(volatile uint16_t*)(uintptr_t)COMMON = (uint16_t)(v))
#define READ_MONITOR(w)                                                        \
    (*(volatile uint16_t*)(uintptr_t)MONITOR = (uint16_t)(w),                  \
     *(volatile uint16_t*)(uintptr_t)MONITOR)
#endif

/* the title's link provides the driver bytes; never the fork */
extern const unsigned char saturn_spu_driver[] __attribute__((weak)),
    saturn_spu_driver_end[] __attribute__((weak));

enum { ST_IDLE, ST_LOADING, ST_READY, ST_OFF };
/* sh2_permit write pending; STOP: a re-key's stop form, then ON */
enum { PERMIT_KEEP, PERMIT_ON, PERMIT_OFF, PERMIT_STOP };

typedef struct { /* our image of _PCM_CTRL; icsr_target is the driver's */
    uint8_t loop_type, bit_depth;
    uint16_t hi, lo, lsa, playsize, pitch;
    uint8_t pan, volume;
    uint16_t bpb;
} ent_t;

static struct {
    int st;
    unsigned frames;
    uint32_t ctrl, dirty; /* entry 0 offset in sound RAM; voice bits */
    int mvol_dirty;
    uint16_t sh[0x100];
    ent_t ent[NV + 2]; /* [NV], [NV + 1]: the stream */
    struct {
        uint8_t keyed, loop, permit;
        uint32_t left; /* one-shot driver passes left, 0 = uncounted */
    } vs[NV + 2];
    struct {
        int on, started, nch;
        uint32_t cur;   /* write cursor, the same in every ring */
        uint32_t lo[2]; /* ring per channel */
        uint32_t len;   /* ring bytes */
        uint32_t blk;   /* CA block bytes */
    } mv;
    saturn_spu_stats_t stats;
} s;

static void off(unsigned* counter) {
    (*counter)++;
    s.st = ST_OFF;
}

/* byte copy into sound RAM, 16-bit words when the offset is even */
static void put(uint32_t o, const uint8_t* src, uint32_t n) {
    uint32_t i = 0;
    if (!(o & 1)) {
        for (; i + 1 < n; i += 2) {
            SET16(o + i, (uint32_t)src[i] << 8 | src[i + 1]);
        }
    }
    for (; i < n; i++) {
        SET8(o + i, src[i]);
    }
}

static unsigned mvol(void) {
    unsigned l = SH(R_MAIN_L), r = SH(R_MAIN_R), m = (l > r ? l : r) >> 10;
    return m > 15 ? 15 : m;
}

static void load(void) {
    uintptr_t b = (uintptr_t)saturn_spu_driver;
    uintptr_t e = (uintptr_t)saturn_spu_driver_end;
    uint32_t o;
    s.st = ST_OFF;
    if (!b || e <= b || e - b > SYS) {
        s.stats.no_driver++;
        return;
    }
    if (saturn_smpc_sndoff()) {
        s.stats.sndoff_fail++;
        return;
    }
    SET_COMMON(0x0200u | mvol());
    for (o = 0; o < CLEAR_END; o += 4) {
        SET32(o, 0);
    }
    put(0, saturn_spu_driver, (uint32_t)(e - b));
    if (saturn_smpc_sndon()) {
        s.stats.sndon_fail++;
        return;
    }
    s.st = ST_LOADING;
    s.frames = 0;
}

static int up(void) {
    if (s.st == ST_IDLE) {
        load();
    }
    return s.st != ST_OFF;
}

/* r = p / 0x1000: OCT = floor(log2 r), FNS = round(1024 (r / 2^OCT - 1)) */
static uint16_t pitch_word(uint32_t p) {
    int e = 13;
    uint32_t fns;
    while (!(p >> e)) {
        e--;
    }
    if (e < 4) { /* below 0x10 OCT stays at the SCSP's -8 */
        return 8u << 11;
    }
    fns = (((p - (1u << e)) << 11) + (1u << e)) >> (e + 1);
    if (fns > 1023) {
        fns = 1023;
    }
    return (uint16_t)(((unsigned)(e - 12) & 0xFu) << 11 | fns);
}

/* round(44100 r) bytes per second of PCM8 / pump frames per second */
static uint16_t bpb(uint32_t p) {
    return (uint16_t)(((44100u * p + 0x800u) >> 12) / SATURN_SPU_FPS);
}

/* fixed-mode level >> 6; a sweep-mode side (bit 15) counts as 0 */
static uint8_t volume(int v, int count) {
    unsigned l = VREG(v, 0), r = VREG(v, 2), m;
    if (count && ((l | r) & 0x8000u)) {
        s.stats.sweep++;
    }
    if (count && l != r) {
        s.stats.stereo++;
    }
    l = l & 0x8000u ? 0 : l;
    r = r & 0x8000u ? 0 : r;
    m = (l > r ? l : r) >> 6;
    return (uint8_t)(m > 255 ? 255 : m);
}

/* one side's level, as volume() */
static uint8_t side(unsigned x) {
    x = x & 0x8000u ? 0 : x >> 6;
    return (uint8_t)(x > 255 ? 255 : x);
}

static void key_on(int v) {
    uint32_t a = (uint32_t)VREG(v, 6) * 8u, ram = a + SPU_TO_RAM, n, p;
    ent_t* en = &s.ent[v];
    int loop;
    if (s.st == ST_OFF) {
        return;
    }
    p = VREG(v, 4) & 0x3FFFu;
    if (!p) {
        s.stats.pitch0++;
        return;
    }
    if (a < REGION_LO || ram + 8 > MOVIE_LO) {
        s.stats.key_refused++;
        return;
    }
    n = RAM16(ram + 2); /* BE sample header: flags, samples, loop start, 0 */
    if (!n || ram + 8 + n > MOVIE_LO) {
        s.stats.key_refused++;
        return;
    }
    loop = RAM16(ram) & 1;
    /* 1 forward loop, 0xFF PCM_PROTECTED (-1) */
    en->loop_type = loop ? 1 : 0xFF;
    en->bit_depth = 1; /* PCM8 */
    en->hi = (uint16_t)((ram + 8) >> 16);
    en->lo = (uint16_t)((ram + 8) & 0xFFFFu);
    en->lsa = loop ? RAM16(ram + 4) : 0;
    en->playsize = (uint16_t)n;
    en->pitch = pitch_word(p);
    en->pan = 0;
    en->volume = volume(v, 1);
    en->bpb = bpb(p);
    s.vs[v].keyed = 1;
    s.vs[v].loop = (uint8_t)loop;
    /* a protected one-shot still counting down only updates at permit 1: re-key
     * it with a stop first (left is set for one-shots only) */
    s.vs[v].permit = s.vs[v].left ? PERMIT_STOP : PERMIT_ON;
    s.dirty |= 1u << v;
}

static void key_off(int v) {
    if (!s.vs[v].keyed) {
        return;
    }
    s.vs[v].keyed = 0;
    if (s.vs[v].loop) {
        s.vs[v].permit = PERMIT_OFF;
    } else {
        s.ent[v].volume = 0;
    }
    s.dirty |= 1u << v;
}

static void keys(unsigned bits, int base, void (*fn)(int)) {
    int i;
    for (i = 0; bits; i++, bits >>= 1) {
        if (bits & 1) {
            fn(base + i);
        }
    }
}

static void voice_write(int v, unsigned r) {
    if (r >= 8) { /* adsr1/adsr2, volumex, loop addr */
        if (r < 0xC) {
            s.stats.adsr++;
        } else if (r == 0xC) {
            s.stats.volumex++;
        } else {
            s.stats.loop_addr++;
        }
        return;
    }
    if (!s.vs[v].keyed || r == 6) { /* addr applies at key-on */
        return;
    }
    if (r == 4) {
        uint32_t p = VREG(v, 4) & 0x3FFFu;
        if (p) {
            s.ent[v].pitch = pitch_word(p);
            s.ent[v].bpb = bpb(p);
        }
    } else {
        s.ent[v].volume = volume(v, 0);
    }
    s.dirty |= 1u << v;
}

void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value) {
    up();
    if (reg_offset >= 0x200u || (reg_offset & 1)) {
        s.stats.bad_offset++;
        return;
    }
    if (reg_offset == R_SPUSTAT) { /* read-only */
        return;
    }
    SH(reg_offset) = value;
    if (reg_offset < R_MAIN_L) {
        voice_write((int)(reg_offset >> 4), reg_offset & 0xFu);
        return;
    }
    switch (reg_offset) {
    case R_MAIN_L:
    case R_MAIN_R:
        s.mvol_dirty = 1;
        break;
    case R_KEY_ON:
        keys(value, 0, key_on);
        break;
    case R_KEY_ON + 2:
        keys(value & 0xFFu, 16, key_on);
        break;
    case R_KEY_OFF:
        keys(value, 0, key_off);
        break;
    case R_KEY_OFF + 2:
        keys(value & 0xFFu, 16, key_off);
        break;
    case R_TRANS_ADDR: /* shadow only: the transfer is Psyz_SpuMemWrite */
        break;
    case R_SPUCNT:
        if (value & ~CNT_MAPPED) {
            s.stats.spucnt_other++;
        }
        break;
    case 0x190u: /* chan_fm */
    case 0x192u:
        s.stats.fm++;
        break;
    case 0x194u: /* noise_mode */
    case 0x196u:
        s.stats.noise++;
        break;
    case 0x184u: /* rev_vol */
    case 0x186u:
    case 0x198u: /* rev_mode */
    case 0x19Au:
    case 0x1A2u: /* rev_work_addr */
        s.stats.reverb++;
        break;
    case 0x1B0u: /* cd_vol */
    case 0x1B2u:
        s.stats.cd_vol++;
        break;
    case 0x1B4u: /* ex_vol */
    case 0x1B6u:
        s.stats.ex_vol++;
        break;
    case 0x1A4u:
        s.stats.irq_addr++;
        break;
    case 0x1ACu:
        s.stats.data_trans++;
        break;
    case 0x1A8u: /* trans_fifo */
        s.stats.fifo++;
        break;
    default:
        if (reg_offset >= 0x1C0u) { /* reverb 0x1C0-0x1FE */
            s.stats.reverb++;
        } else {
            s.stats.other++;
        }
        break;
    }
}

unsigned short Psyz_SpuRead(unsigned int reg_offset) {
    up();
    if (reg_offset >= 0x200u || (reg_offset & 1)) {
        s.stats.bad_offset++;
        return 0;
    }
    if (reg_offset == R_SPUSTAT) { /* spucnt's low 6 bits; no capture bit 11 */
        return SH(R_SPUCNT) & 0x3Fu;
    }
    /* trans_addr: the desktop's (addr << 3) >> 3 is the stored value */
    return SH(reg_offset);
}

void Psyz_SpuMemWrite(unsigned int offset, const void* src, unsigned int size) {
    if (!up()) {
        s.stats.mem_dropped++;
        return;
    }
    if (offset < REGION_LO || offset > REGION_END ||
        size > REGION_END - offset) {
        s.stats.mem_refused++;
        return;
    }
    put(offset + SPU_TO_RAM, src, size);
}

void Psyz_SpuMemRead(unsigned int offset, void* dst, unsigned int size) {
    uint8_t* d = dst;
    uint32_t i, a;
    up();
    for (i = 0; i < size; i++) {
        a = offset + i;
        d[i] = a >= REGION_LO && a < REGION_END ? RAM8(a + SPU_TO_RAM) : 0;
    }
}

int Psyz_SpuVoiceKeyStatus(int voice) {
    up();
    if (voice < 0 || voice >= NV) {
        return -1;
    }
    return s.vs[voice].keyed;
}

static void write_entry(int v) {
    const ent_t* en = &s.ent[v];
    uint32_t o = s.ctrl + ENTRY * (uint32_t)v;
    SET8(o + 0, s.vs[v].permit == PERMIT_STOP ? 1 : en->loop_type);
    SET8(o + 1, en->bit_depth);
    SET16(o + 2, en->hi);
    SET16(o + 4, en->lo);
    SET16(o + 6, en->lsa);
    SET16(o + 8, en->playsize);
    SET16(o + 10, en->pitch);
    SET8(o + 12, en->pan);
    SET8(o + 13, en->volume);
    SET16(o + 14, en->bpb);
    if (s.vs[v].permit == PERMIT_STOP) { /* stop-then-play, two passes: */
        /* a loop at permit 0 is ended (drv main.c:1153-1170, :517-524) */
        SET8(o + 18, 0);
        s.vs[v].left = 0;
        s.vs[v].permit = PERMIT_ON; /* the protected play: next pump */
    } else if (s.vs[v].permit != PERMIT_KEEP) { /* sh2_permit last */
        SET8(o + 18, s.vs[v].permit == PERMIT_ON);
        if (s.vs[v].permit == PERMIT_ON && !s.vs[v].loop) {
            s.vs[v].left = en->bpb ? (en->playsize + en->bpb - 1u) / en->bpb
                                   : 0; /* bpb 0: never ends (driver too) */
        }
        s.vs[v].permit = PERMIT_KEEP;
    }
}

void saturn_spu_pump(void) {
    int v;
    uint32_t carry = 0;
    if (s.st == ST_IDLE || s.st == ST_OFF) {
        return;
    }
    if (s.st == ST_LOADING) {
        s.frames++;
        if (RAM16(SYS_START) != READY) {
            if (s.frames >= SATURN_SPU_READY_FRAMES) {
                off(&s.stats.ready_timeout);
            }
            return;
        }
        s.ctrl = RAM32(SYS_CTRL) - RAM_BASE;
        if (s.ctrl < DRV_LO || s.ctrl > SYS - ENTRIES * ENTRY || (s.ctrl & 1)) {
            off(&s.stats.bad_ctrl);
            return;
        }
        s.st = ST_READY;
        s.dirty = (1u << NV) - 1u; /* key-ons while loading flush here */
        s.mvol_dirty = 1;
    }
    for (v = 0; v < NV; v++) { /* one driver pass per frame */
        if (s.vs[v].keyed && !s.vs[v].loop && s.vs[v].left &&
            s.vs[v].permit == PERMIT_KEEP && !--s.vs[v].left) {
            s.vs[v].keyed = 0;
            s.stats.ended++;
        }
    }
    for (v = 0; v <= NV + 1; v++) { /* NV, NV + 1: dirty after a begin */
        if (s.dirty & (1u << v)) {
            write_entry(v);
            carry |= (uint32_t)(s.vs[v].permit != PERMIT_KEEP) << v;
        }
    }
    s.dirty = carry; /* stop-then-play, two passes */
    if (s.mvol_dirty) {
        SET_COMMON(0x0200u | mvol());
        s.mvol_dirty = 0;
    }
    SET16(SYS_START, 1);
}

/* stream: mono on entry NV, stereo L on NV and R on NV + 1; forward loops
 * over rings ending at ADDR_MAX, 0x8000 B per byte of a sample split per
 * channel; hard left / right pans (DIPAN bit 4: left, ST-077 p0097); the CD
 * volume (SPU 0x1B0/0x1B2, PS1 scales CD input before main volume; main
 * volume reaches the stream once, through MVOL); permit 1 at the first write,
 * 0 at end (the driver frees the slot at permit 0); a first write while that
 * 0 is pending restarts the slot (stop form) */
int saturn_spu_stream_begin(uint32_t rate, int channels, int bits) {
    uint32_t p = (rate * 0x1000u + 22050u) / 44100u, o, bps, span;
    ent_t* en;
    int c;
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16)) {
        return -1;
    }
    if (s.st != ST_READY || s.mv.on || !p) {
        return -1;
    }
    bps = (uint32_t)bits / 8u;
    span = STREAM_SPAN * bps;
    for (o = ADDR_MAX - span; o < ADDR_MAX; o += 2) {
        SET16(o, 0);
    }
    s.mv.nch = channels;
    s.mv.len = span / (uint32_t)channels;
    s.mv.blk = STREAM_BLK * bps;
    for (c = 0; c < channels; c++) {
        en = &s.ent[NV + c];
        s.mv.lo[c] = ADDR_MAX - span + s.mv.len * (uint32_t)c;
        en->loop_type = 1;
        en->bit_depth = bits == 8; /* 1: PCM8 */
        en->hi = (uint16_t)(s.mv.lo[c] >> 16);
        en->lo = (uint16_t)(s.mv.lo[c] & 0xFFFFu);
        en->lsa = 0;
        en->playsize = (uint16_t)(s.mv.len / bps);
        en->pitch = pitch_word(p);
        if (channels == 1) {
            en->pan = 0;
            en->volume = volume(R_CD_VOL >> 4, 0);
        } else {
            en->pan = c ? 0x0F : 0x1F;
            en->volume = side(SH(R_CD_VOL + 2u * (unsigned)c));
        }
        en->bpb = bpb(p);
        s.vs[NV + c].loop = 1;
        s.dirty |= 1u << (NV + c);
    }
    s.mv.on = 1;
    s.mv.started = 0;
    s.mv.cur = 0;
    return 0;
}

/* entry NV's play block offset in its ring (CA, address bits 15..12), -1
 * before the driver gives the stream a slot */
static int play_pos(void) {
    unsigned slot = RAM8(s.ctrl + ENTRY * NV + 19);
    if (slot > 31u) { /* 0xFF: none */
        return -1;
    }
    return (int)(((unsigned)READ_MONITOR(slot << 11) >> 7 & 0xFu) * s.mv.blk &
                 (s.mv.len - 1u));
}

void saturn_spu_stream_write(const uint8_t* l, const uint8_t* r, uint32_t n) {
    const uint8_t* src[2] = {l, r};
    uint32_t k, lead, o, m = s.mv.len - 1u;
    uint32_t lead_min = 2u * s.mv.blk, lead_max = s.mv.len - s.mv.blk;
    int play, i, c;
    if (!s.mv.on) {
        return;
    }
    if (!s.mv.started) {
        s.mv.started = 1;
        for (c = 0; c < s.mv.nch; c++) {
            s.vs[NV + c].permit =
                s.vs[NV + c].permit == PERMIT_OFF ? PERMIT_STOP : PERMIT_ON;
            s.dirty |= 1u << (NV + c);
        }
    }
    play = play_pos();
    if (play >= 0) {
        lead = (s.mv.cur - (uint32_t)play) & m;
        /* behind the head: within lead_min of the play block, whose head
         * may be up to one block past it */
        if (lead < lead_min || lead > lead_max) {
            /* [play, cur) may be unplayed: kept */
            o = lead < lead_min ? s.mv.cur : (uint32_t)play;
            s.mv.cur = ((uint32_t)play + lead_min) & m;
            for (; o != s.mv.cur; o = (o + 1u) & m) {
                for (c = 0; c < s.mv.nch; c++) {
                    SET8(s.mv.lo[c] + o, 0); /* stale bytes the head reaches */
                }
            }
            s.stats.movie_resync++;
        } else if (lead + n > lead_max) { /* ahead: wait for the head */
            for (i = 0; i < STREAM_WAIT_BOUND && play >= 0 &&
                        ((s.mv.cur - (uint32_t)play) & m) + n > lead_max;
                 i++) {
                play = play_pos();
            }
            if (i == STREAM_WAIT_BOUND) {
                s.stats.movie_overrun++;
            }
        }
    }
    while (n) { /* 16-bit words at an even cursor (put), split at the wrap */
        k = s.mv.len - s.mv.cur;
        k = k < n ? k : n;
        for (c = 0; c < s.mv.nch; c++) {
            if (src[c]) {
                put(s.mv.lo[c] + s.mv.cur, src[c], k);
                src[c] += k;
            }
        }
        s.mv.cur = (s.mv.cur + k) & m;
        n -= k;
    }
}

int saturn_spu_stream_lead(void) {
    int play = s.mv.on ? play_pos() : -1;
    if (play < 0) {
        return -1;
    }
    return (int)((s.mv.cur - (uint32_t)play) & (s.mv.len - 1u));
}

void saturn_spu_stream_end(void) {
    int c;
    if (!s.mv.on) {
        return;
    }
    s.mv.on = 0;
    for (c = 0; c < s.mv.nch; c++) {
        s.vs[NV + c].permit = PERMIT_OFF;
        s.dirty |= 1u << (NV + c);
    }
}

/* the movie: mono PCM8 */
int saturn_spu_movie_begin(uint32_t rate) {
    return saturn_spu_stream_begin(rate, 1, 8);
}

void saturn_spu_movie_write(const uint8_t* pcm, uint32_t n) {
    saturn_spu_stream_write(pcm, NULL, n);
}

int saturn_spu_movie_lead(void) { return saturn_spu_stream_lead(); }

void saturn_spu_movie_end(void) { saturn_spu_stream_end(); }

const saturn_spu_stats_t* saturn_spu_stats(void) {
    s.stats.state = (unsigned)s.st;
    s.stats.movie_cur = s.mv.cur;
    return &s.stats;
}

#ifdef SATURN_SPU_HOST
void saturn_spu_host_reset(void) { memset(&s, 0, sizeof s); }
#endif
