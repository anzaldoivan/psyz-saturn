// Saturn (libyaul) VDP1 backend for the Draw_* endpoints.
// Drawn: flat TILE (0x60-0x63) and POLY_F4 (0x28-0x2B); TILE_1, TILE_8,
// TILE_16 (0x68, 0x70, 0x78) as polygons; SPRT, SPRT_1, SPRT_8, SPRT_16 (raw,
// or tint exactly 0x808080; 4-bit, 8-bit and 15-bit pages) as normal sprites
// from a persistent VDP1 texture cache. Semi-transparent and other-tint forms,
// other 0x60-0x7F codes and POLY_FT4 (0x2C-0x2F) are counted unsupported and
// not drawn; any other code, and every endpoint outside this scope, logs and
// aborts, except Draw_SetTexpageMode (decoded) and Draw_LoadImage (stored).
// With a 4 MiB RAM cart that passes the unit check, a 1 MiB PS1 VRAM shadow on
// the cart backs LoadImage, StoreImage, MoveImage and the sprite texels;
// without one, StoreImage and MoveImage are counted and do nothing. The
// texture cache persists across frames, invalidated by every LoadImage and
// MoveImage destination; psyz_cart_stats counts it.
#include <yaul.h>
#include <psyz.h>
#include <libgpu.h>
#include <psyz/log.h>
#include "draw.h"
#include "saturn_movie.h"
#include <stdlib.h>
#include <string.h>

#define MAXPOLY 64

// read from a save state by the host: frames, unsupported, polys,
// load_failed; cart: cart_id (raw ID byte), cart_size (libyaul),
// cart_units, cart_alias, shadow, store_nocart, move_nocart, store_fb
volatile unsigned psyz_saturn_stats[12];

// cache and cost counters, read from a save state by the host: row 0
// cumulative, row k (1-7) a copy of row 0 at the end of commit k. Ticks are
// cpu_frt_count_get() deltas (unit unverified: printed as ticks).
#define CART_STATS 1
enum {
    CS_COMMITS,
    CS_BYTES_CART,  // arena bytes converted with the shadow
    CS_BYTES_STORE, // arena bytes converted from the texture store
    CS_SETUP_TICKS, // cart_detect, the 1 MiB zeroing included
    CS_COPY_TICKS,  // sprite conversions
    CS_FRAME_TICKS, // Draw_ResetBuffer or the previous commit end to commit end
    CS_DIRTY_SPANS,
    CS_INVALIDATIONS,
    CS_HITS,
    CS_LOOKUPS,
    CS_EVICTIONS,
    CS_ARENA_HIGH, // bytes, max start + size
    CS_WRAM_BYTES, // sizeof the backend statics
    CS_CART_BYTES, // 1048576 with the shadow, else 0
    CS_FULL,       // cache allocations refused
    CS_LIVE,       // live keys
};
volatile unsigned psyz_cart_stats[8][16];
#define CS(i) psyz_cart_stats[0][CS_##i]

#ifndef PSYZ_SATURN_TEX_LRU
#define PSYZ_SATURN_TEX_LRU 0 // 1: LRU texture cache, 0: drop-all
#endif

// texture cache policy counters, read from a save state by the host; each
// key column is followed by its byte column
enum {
    TS_POLICY,  // PSYZ_SATURN_TEX_LRU as built
    TS_UPLOADS, // miss conversions
    TS_UP_BYTES,
    TS_EV_CAP, // live keys reclaimed for space
    TS_EV_CAP_BYTES,
    TS_EV_INVAL, // dead keys reclaimed
    TS_EV_INVAL_BYTES,
    TS_EV_REFUSAL, // live keys dropped after a refused allocation
    TS_EV_REFUSAL_BYTES,
    TS_REFUSALS,          // misses refused, counted unsupported
    TS_SLOT_REFUSALS,     // of those, the key table full
    TS_PINNED_AT_REFUSAL, // max keys held for the open list at a refusal
    TS_FREE_LARGEST,      // largest free block at the last commit end
    TS_FRAG_REFUSALS,     // refusals with enough free bytes, none contiguous
    TS_COUNT,
};
volatile unsigned psyz_tex_stats[TS_COUNT];
#define TS(i) psyz_tex_stats[TS_##i]

#ifndef PSYZ_SATURN_FB_FEEDBACK
#define PSYZ_SATURN_FB_FEEDBACK 0 // 1: drawn rects copied back to the shadow
#endif
#if PSYZ_SATURN_FB_FEEDBACK
// framebuffer feedback counters, read from a save state by the host
enum {
    FB_POLICY, // PSYZ_SATURN_FB_FEEDBACK as built
    FB_RECTS,  // rects copied
    FB_BYTES,
    FB_POLLS,       // CEF reads that found the plot unfinished
    FB_TORN,        // plot outlived the field
    FB_UNSUPPORTED, // lists not copied: unsupported, stp texel, area, mask
    FB_PENDING,     // StoreImage/MoveImage source meets an unplotted rect
    FB_MOVE_IN,     // MoveImage into the drawn frame (not drawn)
};
volatile unsigned psyz_fb_stats[8];
#define FB(i) psyz_fb_stats[FB_##i]
#endif

// FRT ticks extended past 16 bits; sampled often enough that no delta wraps
static uint32_t frt_ext;
static uint16_t frt_last;
static uint32_t tick(void) {
    uint16_t c = cpu_frt_count_get();
    frt_ext += (uint16_t)(c - frt_last);
    frt_last = c;
    return frt_ext;
}
static uint32_t frame_start;

static vdp1_cmdt_list_t* list;
static unsigned npoly, unsupported;
static int16_vec2_t sysclip = INT16_VEC2_INITIALIZER(319, 239);
static int16_vec2_t local;
static uint16_t* tex_arena; // VDP1 VRAM: start of libyaul's texture partition
#if !PSYZ_SATURN_TEX_LRU
static unsigned tex_arena_used;    // bytes, reset by the drop-all eviction
static unsigned tex_evict_pending; // drop-all deferred to the end of commit
static unsigned tex_half;          // byte base of the half being filled
static unsigned tex_plot_half; // byte base of the half the plotted list reads
#endif
#define TEX_CACHE_BYTES 262144
static unsigned tex_nkeys; // slots used, dead ones included
#if PSYZ_SATURN_TEX_LRU
static unsigned tex_clock; // +1 per lookup; a hit or an insert stamps the key
static unsigned tex_pin;   // tex_clock at the end of the commit before last
static unsigned tex_pin_next; // tex_clock at the end of the last commit
static unsigned tex_gap(unsigned need, unsigned* big, unsigned* sum);
#else
static void tex_drop(int refusal);
#endif
static unsigned wram_bytes(void);

static void fail(const char* fn) {
    ERRORF("%s is not implemented on Saturn", fn);
    abort();
}

// PS1 VRAM shadow on the cart: halfword (x,y) at SHADOW_AD, native order.
// Accessors are overridable (host test: the fake cart); 16-bit only.
#ifndef SHADOW_R16
#define SHADOW_R8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define SHADOW_R16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define SHADOW_W16(a, v) (*(volatile uint16_t*)(uintptr_t)(a) = (uint16_t)(v))
#endif
#if PSYZ_SATURN_FB_FEEDBACK
// VDP1 draw framebuffer, 16 bpp, stride 512 halfwords
#ifndef FB_R16
#define FB_R16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#endif
#define FB_AD(x, y) VDP1_FB(((y) << 9 | (x)) << 1)
#endif
#define SHADOW_BASE 0x22400000UL
#define SHADOW_AD(x, y)                                                        \
    (SHADOW_BASE +                                                             \
     ((((uint32_t)(y) & 0x1FF) << 10 | ((uint32_t)(x) & 0x3FF)) << 1))
#define CART_ID_ADDR 0x24FFFFFFUL // CS1 ID byte (libyaul dram-cart/map.h)
#define CART_UNIT 0x80000UL
static int cart_done, shadow;

// once: libyaul's detection, then the raw ID byte (only when libyaul saw a
// DRAM cart; its own answer is not trusted: an aliased 0x5C cart reports
// 1 MiB, an unmirrored 0x5A one 4 MiB); ID 0x5C runs the unit check, which
// must read back all eight 512 KiB units distinct, then the shadow is zeroed
static void cart_probe(void) {
    unsigned u, units = 0;
    uint32_t a;
    dram_cart_init();
    if (dram_cart_id_get() != DRAM_CART_ID_INVALID) {
        psyz_saturn_stats[4] = SHADOW_R8(CART_ID_ADDR);
    }
    psyz_saturn_stats[5] = dram_cart_size_get();
    if (psyz_saturn_stats[4] != DRAM_CART_ID_4MIB) {
        return;
    }
    for (u = 0; u < 8; u++) {
        SHADOW_W16(SHADOW_BASE + u * CART_UNIT, 0xA500 | u);
        SHADOW_W16(SHADOW_BASE + u * CART_UNIT + CART_UNIT - 2, 0x5A00 | u);
    }
    for (u = 0; u < 8; u++) {
        units += SHADOW_R16(SHADOW_BASE + u * CART_UNIT) == (0xA500 | u) &&
                 SHADOW_R16(SHADOW_BASE + u * CART_UNIT + CART_UNIT - 2) ==
                     (0x5A00 | u);
    }
    psyz_saturn_stats[6] = units;
    if (units != 8) {
        psyz_saturn_stats[7] = 1;
        return;
    }
    for (a = 0; a < 0x100000; a += 2) {
        if (!(a & 0x3FF)) {
            tick(); // every 512 halfwords
        }
        SHADOW_W16(SHADOW_BASE + a, 0);
    }
    shadow = 1;
    psyz_saturn_stats[8] = 1;
    CS(CART_BYTES) = 0x100000;
}
static void cart_detect(void) {
    uint32_t t0;
    if (cart_done) {
        return;
    }
    cart_done = 1;
    t0 = tick();
    cart_probe();
    CS(SETUP_TICKS) += tick() - t0;
}

// Psy-Q LoadImage/StoreImage clamp: w to [0,1024], h to [0,512]
static void cart_clamp(const RECT* r, int* w, int* h) {
    *w = r->w < 0 ? 0 : r->w > 1024 ? 1024 : r->w;
    *h = r->h < 0 ? 0 : r->h > 512 ? 512 : r->h;
}

// cart-path LoadImage: row-major over the clamped rect, x and y wrap per
// halfword; returns 0 (not taken) without a shadow
static int cart_load(RECT* rect, const u_long* p) {
    const uint16_t* d = (const uint16_t*)p;
    int w, h, i, j;
    cart_detect();
    if (!shadow) {
        return 0;
    }
    cart_clamp(rect, &w, &h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            SHADOW_W16(SHADOW_AD(rect->x + i, rect->y + j), d[j * w + i]);
        }
    }
    return 1;
}
#define CART_LOAD(rect, p) cart_load(rect, p)

// a LoadImage destination (clamped) is one dirty span (tex_dirty, below)
static void tex_dirty(int x, int y, int w, int h);
static void tex_dirty_load(const RECT* r) {
    int w, h;
    cart_clamp(r, &w, &h);
    if (w * h > 0) {
        tex_dirty(r->x, r->y, w, h);
    }
}
#define TEX_DIRTY_LOAD(rect) tex_dirty_load(rect)

#if PSYZ_SATURN_DCPU
// dual SH-2: 1 DUAL, 2
// DUAL_FORCE_DELAY, 3 DUAL_FORCED_SINGLE_FALLBACK; absent or 0 compiles none
// of this. Draw_PushPrim queues flat TILE and POLY_F4 as descriptors that keep
// their list slot; Draw_FlushBuffer publishes them, the slave prepares seq >=
// count/2 and the master the rest, one bounded join, then the merge fills the
// slots in seq order with poly()'s setters. Shared data moves only through
// dcpu_rd/dcpu_wr (the cache-through alias); the slave calls no vdp1, cart,
// SMPC or non-inline libyaul function.
#define DCPU_STATS 1
#define DCPU_JOIN_BOUND 16384 // master FRT ticks (DIV_8, unit unverified)
#define DCPU_DELAY 2048       // slave FRT ticks, DUAL_FORCE_DELAY only
enum {
    DC_MODE,
    DC_EMITS,
    DC_DESC_DONE,
    DC_BATCHES,
    DC_NOTIFIES,
    DC_ICF_SEEN,
    DC_WAIT_ITERS,
    DC_TIMEOUTS,
    DC_FALLBACKS,
    DC_GEN,
    DC_GEN_FAIL,
    DC_COUNT_FAIL,
    DC_CSUM_FAIL,
    DC_TICKS_A,
    DC_TICKS_B,
    DC_BYTES,
};
typedef struct {
    uint32_t w[5];      // the packet words as dispatched, rest 0
    uint16_t seq, kind; // kind 0 TILE, 1 POLY_F4
    uint32_t pad[2];
} dcpu_desc_t;
typedef struct {
    uint16_t seq, gen, colour, kind;
    int16_t xy[4][2]; // VDP1 order, after the ring and the -1 rule
    uint32_t csum, pad;
} dcpu_rec_t;
typedef struct { // batch header, master -> slave
    uint16_t gen, count, first, pad;
    uint32_t desc_csum, pad2;
} dcpu_bhdr_t;
typedef struct { // done header, slave -> master
    uint16_t gen, done;
    uint32_t desc_csum_echo, rec_csum, pad;
} dcpu_dhdr_t;
struct dcpu_desc_region {
    dcpu_bhdr_t h;
    dcpu_desc_t d[MAXPOLY];
};
struct dcpu_res_region {
    dcpu_dhdr_t h;
    dcpu_rec_t r[MAXPOLY];
};
_Static_assert(sizeof(struct dcpu_desc_region) == 2064, "desc region");
_Static_assert(sizeof(struct dcpu_res_region) == 2064, "res region");
// read from a save state by ELF symbol; row 0 master, row 1 slave
struct dcpu_desc_region psyz_dcpu_desc __attribute__((aligned(16)));
struct dcpu_res_region psyz_dcpu_res __attribute__((aligned(16)));
volatile unsigned psyz_dcpu_stats[2][16] __attribute__((aligned(16)));

// shared accesses, both CPUs: volatile through the cache-through alias, so no
// line of a region is ever cached; overridable (host test: recording stubs)
#ifndef dcpu_rd32
#define DCPU_A(p) ((uintptr_t)(p) | CPU_CACHE_THROUGH)
#define dcpu_rd32(p) (*(volatile uint32_t*)DCPU_A(p))
#define dcpu_rd16(p) (*(volatile uint16_t*)DCPU_A(p))
#define dcpu_wr32(p, v) (*(volatile uint32_t*)DCPU_A(p) = (uint32_t)(v))
#define dcpu_wr16(p, v) (*(volatile uint16_t*)DCPU_A(p) = (uint16_t)(v))
#endif
// the master's FTCSR ICF (SINIT from the slave), cleared when seen
#ifndef DCPU_ICF_TAKE
static inline int dcpu_icf_take(void) {
    volatile cpu_ioregs_t* const r = (volatile cpu_ioregs_t*)CPU_IOREG_BASE;
    if (!(r->ftcsr & 0x80)) {
        return 0;
    }
    r->ftcsr &= ~0x80;
    return 1;
}
#define DCPU_ICF_TAKE() dcpu_icf_take()
#endif

static const int dcpu_on = PSYZ_SATURN_DCPU != 3;
static dcpu_desc_t mdesc[MAXPOLY]; // master-private: the queue, by seq
static uint8_t dslot[MAXPOLY];     // the list slot of each seq
static dcpu_rec_t mrec[MAXPOLY];   // master-private records, merged by seq
static unsigned ndesc;

static void dcpu_add(unsigned row, unsigned i, unsigned n) {
    dcpu_wr32(
        &psyz_dcpu_stats[row][i], dcpu_rd32(&psyz_dcpu_stats[row][i]) + n);
}
// rotate-add: a plain sum would miss swapped words
static uint32_t dcpu_csum(uint32_t c, uint32_t w) {
    return (c << 1 | c >> 31) + w;
}
static uint32_t dcpu_rsum(const dcpu_rec_t* r) {
    uint32_t c = dcpu_csum(0, (uint32_t)r->seq << 16 | r->gen);
    int i;
    c = dcpu_csum(c, (uint32_t)r->colour << 16 | r->kind);
    for (i = 0; i < 4; i++) {
        c = dcpu_csum(
            c, (uint32_t)(uint16_t)r->xy[i][0] << 16 | (uint16_t)r->xy[i][1]);
    }
    return c;
}
static uint32_t dcpu_dsum(uint32_t c, const dcpu_desc_t* d) {
    int i;
    for (i = 0; i < 5; i++) {
        c = dcpu_csum(c, d->w[i]);
    }
    c = dcpu_csum(c, (uint32_t)d->seq << 16 | d->kind);
    return dcpu_csum(dcpu_csum(c, 0), 0); // the two pad words
}

// both CPUs: the mapping of color(), tile() and Draw_PushPrim's POLY_F4
static void dcpu_prep(const dcpu_desc_t* d, uint16_t gen, dcpu_rec_t* r) {
    const uint32_t* w = d->w;
    int16_t x[4], y[4], mx, my;
    int i;
    r->seq = d->seq;
    r->gen = gen;
    r->kind = d->kind;
    r->colour =
        0x8000 | (uint16_t)((w[0] >> 19) & 31) << 10 |
        (uint16_t)((w[0] >> 11) & 31) << 5 | (uint16_t)((w[0] >> 3) & 31);
    if (!d->kind) {
        int x0 = (short)(w[1] & 0xFFFF), y0 = (short)(w[1] >> 16);
        int ww = (short)(w[2] & 0xFFFF), hh = (short)(w[2] >> 16);
        x[0] = x[3] = x0;
        x[1] = x[2] = x0 + ww - 1;
        y[0] = y[1] = y0;
        y[2] = y[3] = y0 + hh - 1;
    } else {
        static const int ring[4] = {1, 2, 4, 3};
        for (i = 0; i < 4; i++) {
            x[i] = (short)(w[ring[i]] & 0xFFFF);
            y[i] = (short)(w[ring[i]] >> 16);
        }
        mx = x[0];
        my = y[0];
        for (i = 1; i < 4; i++) {
            if (x[i] > mx) {
                mx = x[i];
            }
            if (y[i] > my) {
                my = y[i];
            }
        }
        for (i = 0; i < 4; i++) {
            x[i] = x[i] == mx ? x[i] - 1 : x[i];
            y[i] = y[i] == my ? y[i] - 1 : y[i];
        }
    }
    for (i = 0; i < 4; i++) {
        r->xy[i][0] = x[i];
        r->xy[i][1] = y[i];
    }
    r->csum = dcpu_rsum(r);
    r->pad = 0;
}

// Draw_PushPrim: queue a flat TILE (w, h > 0) or POLY_F4, taking poly()'s
// list slot under poly()'s overflow rule; 0 = not queued (the original path)
static int dcpu_queue(const u_long* packets, unsigned code) {
    unsigned k, n = code >= 0x60 ? 3 : 5;
    dcpu_desc_t* d = &mdesc[ndesc];
    if (n == 3 && ((short)(packets[2] & 0xFFFF) <= 0 ||
                   (short)((packets[2] >> 16) & 0xFFFF) <= 0)) {
        return 0;
    }
    if (npoly >= MAXPOLY) {
        unsupported++;
        return 1;
    }
    // ndesc <= npoly < MAXPOLY, so a batch always fits the regions
    for (k = 0; k < 5; k++) {
        d->w[k] = k < n ? (uint32_t)(packets[k] & 0xFFFFFFFFu) : 0;
    }
    d->seq = ndesc;
    d->kind = n == 5;
    dslot[ndesc++] = npoly++;
    return 1;
}

// the slave's own FRT ticks since *last (no shared state)
static uint32_t dcpu_stick(uint16_t* last) {
    uint16_t c = cpu_frt_count_get(), d = (uint16_t)(c - *last);
    *last = c;
    return d;
}

// slave entry (libyaul's polling loop, after MINIT): read the batch header and
// the slave's descriptors, prepare, write records and the done header, SINIT
static void dcpu_slave_entry(void) {
    struct dcpu_desc_region* D = &psyz_dcpu_desc;
    struct dcpu_res_region* R = &psyz_dcpu_res;
    uint16_t last = cpu_frt_count_get(), gen, count, first, q;
    uint32_t work = 0, echo = 0, rs = 0;
    dcpu_desc_t d;
    dcpu_rec_t r;
    int k;
    dcpu_wr32(&psyz_dcpu_stats[1][DC_MODE], cpu_dual_executor_get());
    dcpu_add(1, DC_ICF_SEEN, 1);
    gen = dcpu_rd16(&D->h.gen);
    count = dcpu_rd16(&D->h.count);
    first = dcpu_rd16(&D->h.first);
    dcpu_add(1, DC_BATCHES, 1);
    dcpu_add(1, DC_BYTES, 16 + 32 * (count - first));
#if PSYZ_SATURN_DCPU == 2
    {
        uint32_t spent = 0;
        work += dcpu_stick(&last);
        while (spent < DCPU_DELAY) {
            spent += dcpu_stick(&last);
        }
        dcpu_add(1, DC_TICKS_B, spent);
    }
#endif
    for (q = first; q < count; q++) {
        dcpu_rec_t* o = &R->r[q - first];
        for (k = 0; k < 5; k++) {
            d.w[k] = dcpu_rd32(&D->d[q].w[k]);
        }
        d.seq = dcpu_rd16(&D->d[q].seq);
        d.kind = dcpu_rd16(&D->d[q].kind);
        echo = dcpu_dsum(echo, &d);
        dcpu_prep(&d, gen, &r);
        dcpu_wr16(&o->seq, r.seq);
        dcpu_wr16(&o->gen, r.gen);
        dcpu_wr16(&o->colour, r.colour);
        dcpu_wr16(&o->kind, r.kind);
        for (k = 0; k < 8; k++) {
            dcpu_wr16(&o->xy[k >> 1][k & 1], r.xy[k >> 1][k & 1]);
        }
        dcpu_wr32(&o->csum, r.csum);
        rs = dcpu_csum(rs, r.csum);
    }
    dcpu_add(1, DC_DESC_DONE, count - first);
    dcpu_add(1, DC_BYTES, 32 * (count - first));
    work += dcpu_stick(&last);
    dcpu_add(1, DC_TICKS_A, work);
    // completion: the done header, then SINIT (the master's ICF)
    dcpu_wr16(&R->h.gen, gen);
    dcpu_wr16(&R->h.done, count - first);
    dcpu_wr32(&R->h.desc_csum_echo, echo);
    dcpu_wr32(&R->h.rec_csum, rs);
    dcpu_wr32(&psyz_dcpu_stats[1][DC_GEN], gen);
    dcpu_add(1, DC_BYTES, 16);
    dcpu_add(1, DC_NOTIFIES, 1);
    cpu_dual_master_notify();
}

// ensure_init, once: polling mode is libyaul's startup state (cpu_init.c), so
// only the entry is installed
static void dcpu_init(void) {
    dcpu_wr32(&psyz_dcpu_stats[0][DC_MODE], PSYZ_SATURN_DCPU);
    if (dcpu_on) {
        if (cpu_dual_executor_get() != 0) {
            fail("dcpu_init on the slave");
        }
        cpu_dual_slave_set(dcpu_slave_entry);
    }
}

// the master prepares seq [a, b) itself
static void dcpu_own(unsigned a, unsigned b, uint16_t gen) {
    unsigned q;
    for (q = a; q < b; q++) {
        dcpu_prep(&mdesc[q], gen, &mrec[q]);
    }
    dcpu_add(0, DC_DESC_DONE, b - a);
}

// after ICF: the done header and the slave's records through the alias, each
// placed in mrec by its seq; nonzero = rejected (counted)
static unsigned dcpu_check(
    uint16_t gen, unsigned first, unsigned count, uint32_t dsum) {
    struct dcpu_res_region* R = &psyz_dcpu_res;
    unsigned n = count - first, i, k, gf, cf, sf;
    uint16_t g = dcpu_rd16(&R->h.gen);
    unsigned done = dcpu_rd16(&R->h.done);
    uint32_t echo = dcpu_rd32(&R->h.desc_csum_echo);
    uint32_t rs = dcpu_rd32(&R->h.rec_csum), c = 0;
    uint64_t seen = 0;
    dcpu_rec_t t;
    dcpu_add(0, DC_ICF_SEEN, 1);
    gf = g != gen;
    cf = done != n;
    sf = echo != dsum;
    for (i = 0; i < n; i++) {
        t.seq = dcpu_rd16(&R->r[i].seq);
        t.gen = dcpu_rd16(&R->r[i].gen);
        t.colour = dcpu_rd16(&R->r[i].colour);
        t.kind = dcpu_rd16(&R->r[i].kind);
        for (k = 0; k < 8; k++) {
            t.xy[k >> 1][k & 1] =
                (int16_t)dcpu_rd16(&R->r[i].xy[k >> 1][k & 1]);
        }
        t.csum = dcpu_rd32(&R->r[i].csum);
        gf |= t.gen != gen;
        sf |= t.csum != dcpu_rsum(&t);
        c = dcpu_csum(c, t.csum);
        if (t.seq < first || t.seq >= count || (seen >> (t.seq & 63) & 1)) {
            cf = 1; // not a permutation of the slave's seqs
            continue;
        }
        seen |= (uint64_t)1 << t.seq;
        mrec[t.seq] = t;
    }
    sf |= rs != c;
    dcpu_add(0, DC_BYTES, 16 + 32 * n);
    dcpu_add(0, DC_GEN_FAIL, gf);
    dcpu_add(0, DC_COUNT_FAIL, cf);
    dcpu_add(0, DC_CSUM_FAIL, sf);
    return gf | cf | sf;
}

// Draw_FlushBuffer, before commit(): publish, kick, the master's half, the
// one bounded wait (the join), the fallback, then the merge in seq order
static void dcpu_flush(void) {
    struct dcpu_desc_region* D = &psyz_dcpu_desc;
    struct dcpu_res_region* R = &psyz_dcpu_res;
    unsigned count = ndesc, first = count / 2, q, k, iters = 0, icf;
    uint16_t gen = (uint16_t)dcpu_rd32(&psyz_dcpu_stats[0][DC_GEN]);
    uint32_t t0, tk, dsum = 0;
    ndesc = 0;
    if (!dcpu_on) {
        if (npoly || unsupported) {
            dcpu_add(0, DC_FALLBACKS, 1);
        }
        return;
    }
    if (!count) {
        return;
    }
    t0 = tick();
    if (dcpu_rd16(&R->h.gen) != gen) { // the slave is not idle: not published
        dcpu_add(0, DC_FALLBACKS, 1);
        dcpu_own(0, count, gen);
    } else {
        gen++;
        for (q = 0; q < count; q++) {
            for (k = 0; k < 5; k++) {
                dcpu_wr32(&D->d[q].w[k], mdesc[q].w[k]);
            }
            dcpu_wr16(&D->d[q].seq, mdesc[q].seq);
            dcpu_wr16(&D->d[q].kind, mdesc[q].kind);
            if (q >= first) {
                dsum = dcpu_dsum(dsum, &mdesc[q]);
            }
        }
        dcpu_wr16(&D->h.gen, gen);
        dcpu_wr16(&D->h.count, count);
        dcpu_wr16(&D->h.first, first);
        dcpu_wr32(&D->h.desc_csum, dsum);
        dcpu_add(0, DC_BATCHES, 1);
        dcpu_wr32(&psyz_dcpu_stats[0][DC_GEN], gen);
        dcpu_add(0, DC_BYTES, 16 + 32 * count);
        dcpu_add(0, DC_NOTIFIES, 1);
        tk = tick();
        dcpu_add(0, DC_TICKS_A, tk - t0);
        cpu_dual_slave_notify();
        dcpu_own(0, first, gen);
        while (!(icf = DCPU_ICF_TAKE()) && tick() - tk < DCPU_JOIN_BOUND) {
            iters++;
        }
        dcpu_add(0, DC_WAIT_ITERS, iters);
        dcpu_add(0, DC_TICKS_B, tick() - tk); // kick to completion
        if (!icf) {
            dcpu_add(0, DC_TIMEOUTS, 1);
        }
        if (!icf || dcpu_check(gen, first, count, dsum)) {
            dcpu_add(0, DC_FALLBACKS, 1);
            dcpu_own(first, count, gen);
        }
    }
    for (q = 0; q < count; q++) { // the merge: poly()'s setters, seq order
        vdp1_cmdt_t* c = &list->cmdts[2 + dslot[q]];
        vdp1_cmdt_draw_mode_t dm = {.raw = 0};
        rgb1555_t col;
        int16_vec2_t v[4];
        col.raw = mrec[q].colour;
        for (k = 0; k < 4; k++) {
            v[k].x = mrec[q].xy[k][0];
            v[k].y = mrec[q].xy[k][1];
        }
        vdp1_cmdt_polygon_set(c);
        dm.pre_clipping_disable = 1;
        vdp1_cmdt_draw_mode_set(c, dm);
        vdp1_cmdt_color_set(c, col);
        vdp1_cmdt_vtx_set(c, v);
    }
}
#endif

static void ensure_init(void) {
    vdp1_env_t env;
    vdp1_vram_partitions_t vp;
    cart_detect();
    if (list) {
        return;
    }
    CS(WRAM_BYTES) = wram_bytes();
    TS(POLICY) = PSYZ_SATURN_TEX_LRU;
#if PSYZ_SATURN_FB_FEEDBACK
    FB(POLICY) = PSYZ_SATURN_FB_FEEDBACK;
#endif
    vdp1_vram_partitions_get(&vp);
    tex_arena = vp.texture_base;
    vdp1_env_default_init(&env);
    env.bpp = VDP1_ENV_BPP_16;
    env.rotation = VDP1_ENV_ROTATION_0;
    env.color_mode = VDP1_ENV_COLOR_MODE_RGB_PALETTE;
    env.erase_color.raw = 0x0000;
    env.erase_points[0].x = 0;
    env.erase_points[0].y = 0;
    env.erase_points[1].x = 319;
    env.erase_points[1].y = 239;
    vdp1_env_set(&env);
#if PSYZ_SATURN_DCPU
    dcpu_init();
#endif
    list = vdp1_cmdt_list_alloc(2 + MAXPOLY + 1);
}

static rgb1555_t color(unsigned w0) {
    rgb1555_t v;
    v.raw = 0x8000 | (uint16_t)((w0 >> 19) & 31) << 10 |
            (uint16_t)((w0 >> 11) & 31) << 5 | (uint16_t)((w0 >> 3) & 31);
    return v;
}

static void poly(unsigned w0, const int16_vec2_t* v) {
    vdp1_cmdt_t* c;
    vdp1_cmdt_draw_mode_t dm = {.raw = 0};
    if (npoly >= MAXPOLY) {
        unsupported++; // overflow is reported as unsupported, not dropped
        return;
    }
    c = &list->cmdts[2 + npoly++];
    vdp1_cmdt_polygon_set(c);
    dm.pre_clipping_disable = 1;
    vdp1_cmdt_draw_mode_set(c, dm);
    vdp1_cmdt_color_set(c, color(w0));
    vdp1_cmdt_vtx_set(c, v);
}

#if PSYZ_SATURN_FB_FEEDBACK
// rects of the open list whose every pixel the list writes exactly (tiles,
// hole-free sprites), before the local offset and the clip
static RECT fb_rect[MAXPOLY];
static unsigned fb_n;
static int fb_void;          // the open list has a bit-15 texel
static int fb_area, fb_mask; // draw area off (0,0), mask bits on
static int fb_hole, fb_stp;  // texel() since the last reset: 0, bit 15

static void fb_note(int x, int y, int w, int h) {
    if (fb_n < MAXPOLY) {
        setRECT(&fb_rect[fb_n], x, y, w, h);
        fb_n++;
    }
}

static int fb_meets(
    int x, int y, int w, int h, int rx, int ry, int rw, int rh) {
    return x < rx + rw && rx < x + w && y < ry + rh && ry < y + h;
}

// a read of the shadow the open list would have drawn over on the PS1
static int fb_pending(int x, int y, int w, int h) {
    unsigned i;
    for (i = 0; npoly && i < fb_n; i++) {
        RECT* r = &fb_rect[i];
        if (fb_meets(x, y, w, h, r->x + local.x, r->y + local.y, r->w, r->h)) {
            return 1;
        }
    }
    return 0;
}

// after vdp1_sync_wait: once the plot ends (CEF) and before the field ends
// (the banks change), the rects go from the draw framebuffer to the shadow;
// RGB writes set bit 15, the PS1 mask bit is 0
static void fb_copy(void) {
    unsigned i, n = fb_n;
    int x, y, x1, y1;
    uint16_t v0, v;
    RECT* r;
    fb_n = 0;
    if (!shadow) {
        fb_void = 0;
        return;
    }
    if (unsupported || fb_void || fb_area || fb_mask) {
        fb_void = 0;
        FB(UNSUPPORTED)++;
        return;
    }
    if (!n) {
        return;
    }
    v0 = saturn_frame_vcount();
    while (!vdp1_transfer_status_get().cef) {
        FB(POLLS)++;
        v = saturn_frame_vcount();
        if (v < v0) {
            FB(TORN)++;
            return;
        }
        v0 = v;
    }
    for (i = 0, r = fb_rect; i < n; i++, r++) {
        x = r->x + local.x < 0 ? 0 : r->x + local.x;
        y = r->y + local.y < 0 ? 0 : r->y + local.y;
        x1 = r->x + local.x + r->w - 1;
        y1 = r->y + local.y + r->h - 1;
        x1 = x1 > sysclip.x ? sysclip.x : x1 > 319 ? 319 : x1;
        y1 = y1 > sysclip.y ? sysclip.y : y1 > 239 ? 239 : y1;
        setRECT(r, x, y, x1 < x ? 0 : x1 - x + 1, y1 < y ? 0 : y1 - y + 1);
        for (y = r->y; y < r->y + r->h; y++) {
            for (x = r->x; x < r->x + r->w; x++) {
                SHADOW_W16(SHADOW_AD(x, y), FB_R16(FB_AD(x, y)) & 0x7FFF);
            }
        }
        FB(RECTS) += r->w && r->h;
        FB(BYTES) += r->w * r->h * 2;
    }
    if (!vdp1_transfer_status_get().cef) {
        FB(TORN)++;
    }
    for (i = 0, r = fb_rect; i < n; i++, r++) {
        if (r->w && r->h) {
            tex_dirty(r->x, r->y, r->w, r->h);
        }
    }
}
#endif

static void tile(unsigned w0, int x, int y, int w, int h) {
    int16_vec2_t v[4] = {
        {x, y}, {x + w - 1, y}, {x + w - 1, y + h - 1}, {x, y + h - 1}};
#if PSYZ_SATURN_FB_FEEDBACK
    unsigned n = npoly;
#endif
    poly(w0, v);
#if PSYZ_SATURN_FB_FEEDBACK
    if (npoly > n) {
        fb_note(x, y, w, h);
    }
#endif
}

static void commit(void) {
    vdp1_cmdt_t* c = list->cmdts;
    uint32_t t;
    unsigned i;
    memset(c, 0, sizeof(vdp1_cmdt_t) * 2);
    vdp1_cmdt_system_clip_coord_set(&c[0]);
    vdp1_cmdt_vtx_system_clip_coord_set(&c[0], sysclip);
    vdp1_cmdt_local_coord_set(&c[1]);
    vdp1_cmdt_vtx_local_coord_set(&c[1], local);
    memset(&c[2 + npoly], 0, sizeof(vdp1_cmdt_t));
    vdp1_cmdt_end_set(&c[2 + npoly]);
    list->count = 2 + npoly + 1;
    vdp1_sync_cmdt_list_put(list, 0);
    vdp1_sync_render();
    vdp1_sync();
    vdp1_sync_wait();
#if PSYZ_SATURN_FB_FEEDBACK
    fb_copy();
#endif
    psyz_saturn_stats[0]++;
#if PSYZ_SATURN_DCPU
    dcpu_add(cpu_dual_executor_get(), DC_EMITS, 1); // row 1 0: master emits
#endif
    psyz_saturn_stats[1] = unsupported;
    psyz_saturn_stats[2] = npoly;
    npoly = 0;
    unsupported = 0;
#if PSYZ_SATURN_TEX_LRU
    {
        unsigned big, sum;
        tex_pin = tex_pin_next; // the plotted list still reads its keys
        tex_pin_next = tex_clock;
        tex_gap(TEX_CACHE_BYTES, &big, &sum);
        TS(FREE_LARGEST) = big;
    }
#else
    tex_plot_half = tex_half; // the plotted list reads only this half
    if (tex_evict_pending) {
        tex_drop(1);
    }
    // half-mark drop-all keeps a list of <= 32 new keys / 64 KiB
    // from filling mid-list; dead-slot reuse or LRU if the hit rate needs it
    if (tex_nkeys > MAXPOLY / 2 || tex_arena_used > TEX_CACHE_BYTES / 4) {
        tex_drop(0);
    }
    TS(FREE_LARGEST) =
        TEX_CACHE_BYTES / 2 + 16 - ((tex_arena_used + 2 + 15) & ~15u);
#endif
    t = tick();
    CS(FRAME_TICKS) += t - frame_start;
    frame_start = t;
    if (++CS(COMMITS) < 8) {
        for (i = 0; i < 16; i++) {
            psyz_cart_stats[CS(COMMITS)][i] = psyz_cart_stats[0][i];
        }
    }
}

void Draw_Reset(void) { fail("Draw_Reset"); }

// SetDispMask(1) (GP1(03h)): the Saturn display is always on, so on=1 carries
// nothing, as Draw_DisplayArea
void Draw_DisplayEnable(unsigned int on) {
    if (on) {
        return;
    }
    fail("Draw_DisplayEnable");
}

// the display is fixed 320x240, so the PS1 area and range calls carry nothing
void Draw_DisplayArea(unsigned int x, unsigned int y) {
    (void)x;
    (void)y;
}

void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}

void Draw_DisplayVerticalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}

// `mode` points at the PS1 GP1(08h) word as a u32 value (core libgpu.c:480
// `(DisplayMode*)&cmd`). The draw.h:23-32 bit-fields assume LSB-first
// allocation; big-endian SH GCC allocates them MSB-first (hres at bits 31-30),
// so a valid 320x240 word read 0 and aborted. Read the u32 and decode the
// PS1 bits with shifts: hres 0-1, vres 2, pal 3, rgb24 4, interlace 5,
// hres368 6, reversed 7. pal, hres368 and reversed stay unchecked, as before.
static int display_on; // set by Draw_SetDisplayMode; gates the movie plane
void Draw_SetDisplayMode(DisplayMode* mode) {
    int i;
    unsigned int w;
    memcpy(&w, mode, sizeof w);
    if ((w & 3) != 1 || ((w >> 2) & 1) != 0 || ((w >> 4) & 1) ||
        ((w >> 5) & 1)) {
        ERRORF("display mode other than 320x240 15-bit progressive");
        abort();
    }
    ensure_init();
    vdp2_tvmd_display_res_set(
        VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A, VDP2_TVMD_VERT_240);
    vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), RGB1555(1, 0, 0, 8));
    for (i = 0; i < 8; i++) {
        vdp2_sprite_priority_set(i, 6);
    }
    vdp2_tvmd_display_set();
    display_on = 1;
    vdp2_sync();
    vdp2_sync_wait();
}

// MOVIE PLANE SEAM BEGIN (saturn_movie.h). begin is refused until
// Draw_SetDisplayMode has turned the display on, saves libyaul's VDP2 shadow
// whole and sets the libyaul_cinepak player's NBG0 screen (main.c:193-290, TVMD
// and back colour left as Draw_SetDisplayMode set them); end writes the shadow
// back whole. Frames go by VDP DMA, run by libyaul's VBlank-in handler
// (vdp_sync.c _vblank_in_handler: the VBlank-in callback, then the queued SCU
// DMA). Between begin and end that callback counts VBlank-ins: the movie clock
// (saturn_gpu_movie_vblanks), which no long decode can make lose a VBlank.
#define MOVIE_FRAME_BYTES (320 * 240 * 2)
#define MOVIE_LINE_SCROLL VDP2_VRAM_ADDR(2, 0x00000)
#define MOVIE_VCS VDP2_VRAM_ADDR(2, 0x10000)
static vdp2_ioregs_t movie_regs;
static int movie_on;
static volatile uint32_t movie_vblanks;

static void movie_vblank_in(void* work) {
    (void)work;
    movie_vblanks++;
}

int saturn_gpu_movie_begin(void) {
    static const vdp2_scrn_bitmap_format_t format = {
        .scroll_screen = VDP2_SCRN_NBG0,
        .ccc = VDP2_SCRN_CCC_RGB_32768,
        .bitmap_size = VDP2_SCRN_BITMAP_SIZE_512X256,
        .palette_base = 0x00000000,
        .bitmap_base = VDP2_VRAM_ADDR(0, 0x00000)};
    static const vdp2_scrn_ls_format_t ls_format = {
        .scroll_screen = VDP2_SCRN_NBG0,
        .table_base = MOVIE_LINE_SCROLL,
        .interval = 0,
        .type = VDP2_SCRN_LS_TYPE_HORZ | VDP2_SCRN_LS_TYPE_VERT};
    static const vdp2_scrn_vcs_format_t vcs_format = {
        .scroll_screen = VDP2_SCRN_NBG0, .table_base = MOVIE_VCS};
    // main.c:208-245 as raw words, T0 in the top nibble: 0x4 NBG0 character
    // pattern, 0xC NBG0 vertical cell scroll, 0xF no access
    static const vdp2_vram_cycp_t cycp = {
        .pt = {{.raw = 0x4444FFFF},
               {.raw = 0x4444FFFF},
               {.raw = 0xC444FFFF},
               {.raw = 0x444FFFFF}}};
    volatile uint32_t* ls = (volatile uint32_t*)MOVIE_LINE_SCROLL;
    volatile uint32_t* vcs = (volatile uint32_t*)MOVIE_VCS;
    uint32_t n, i, m;
    if (!display_on || movie_on) {
        return -1;
    }
    movie_regs = *vdp2_regs_get();
    vdp2_scrn_sf_set(
        VDP2_SCRN_NBG0, 0, VDP2_SCRN_SF_TYPE_NONE, VDP2_SCRN_SF_CODE_A);
    vdp2_scrn_bitmap_format_set(&format);
    vdp2_scrn_priority_set(VDP2_SCRN_NBG0, 3);
    vdp2_scrn_display_set(VDP2_SCRN_DISPTP_NBG0);
    vdp2_vram_cycp_set(&cycp);
    vdp2_scrn_ls_set(&ls_format);
    vdp2_scrn_vcs_set(&vcs_format);
    // remap the 320-pitch frame onto the 512-pitch bitmap (main.c:263-287,
    // as razor85 wrote it)
    for (n = 0; n < 240; n++) {
        ls[n * 2] = (65536u * 320u * n) & 0x01FF0000u;
        i = (128u * 320u * n) & 0x01FF0000u;
        m = 64u * (i / 65536u + 1u) >= 40u * (n + 1u)
                ? 64u
                : 64u * (i / 65536u + 1u) - 40u * n;
        ls[n * 2 + 1] = i + 1024u * (64u - m);
    }
    for (n = 0; n < 256 * 64; n++) {
        vcs[n] = 1024u * (n & 63u);
    }
    movie_vblanks = 0;
    vdp_sync_vblank_in_set(movie_vblank_in, NULL);
    movie_on = 1;
    return 0;
}

int saturn_gpu_movie_frame(const uint16_t* buf) {
    if (!movie_on || vdp_dma_count_get() > 0) {
        return 1;
    }
    vdp_dma_enqueue((void*)VDP2_VRAM_ADDR(0, 0), buf, MOVIE_FRAME_BYTES);
    return 0;
}

uint32_t saturn_gpu_movie_vblanks(void) { return movie_vblanks; }

void saturn_gpu_movie_end(void) {
    if (!movie_on) {
        return;
    }
    vdp_sync_vblank_in_clear();
    vdp2_scrn_display_set(vdp2_scrn_display_get() & ~VDP2_SCRN_DISPTP_NBG0);
    vdp2_vram_cycp_clear();
    *vdp2_regs_get() = movie_regs;
    vdp2_sync();
    movie_on = 0;
}
// MOVIE PLANE SEAM END

// `p` points at the PS1 GP0(E1h) word as a u32 value (core libgpu.c:332
// `(ParamDrawTexpageMode*)&op`). The draw.h:10-20 bit-fields assume LSB-first
// allocation, which big-endian SH GCC does not give, as for the display mode
// above (Draw_SetDisplayMode). Read the u32 and decode the PS1 bits with
// shifts: tx 0-3, ty 4, abr 5-6, tpf 7-8, dtd 9, dfe 10.
static struct TexpageState {
    unsigned tx, ty, abr, tpf, dtd, dfe;
} texpage;
void Draw_SetTexpageMode(ParamDrawTexpageMode* p) {
    unsigned int w;
    memcpy(&w, p, sizeof w);
    texpage.tx = w & 15;
    texpage.ty = (w >> 4) & 1;
    texpage.abr = (w >> 5) & 3;
    texpage.tpf = (w >> 7) & 3;
    texpage.dtd = (w >> 9) & 1;
    texpage.dfe = (w >> 10) & 1;
}

// flat primitives are not textured; sprites ignore the texture window

void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {
    (void)mask_x;
    (void)mask_y;
    (void)off_x;
    (void)off_y;
}

void Draw_SetAreaStart(int x, int y) {
    // system clip is lower-right only, the upper-left stays (0,0)
    (void)x;
    (void)y;
#if PSYZ_SATURN_FB_FEEDBACK
    fb_area = x || y;
#endif
}

void Draw_SetAreaEnd(int x, int y) {
    sysclip.x = x;
    sysclip.y = y;
}

void Draw_SetOffset(int x, int y) {
    local.x = x;
    local.y = y;
}

void Draw_SetMask(int bit0, int bit1) {
    if (bit0 || bit1) {
        WARNF("mask bits are not applied on Saturn");
    }
#if PSYZ_SATURN_FB_FEEDBACK
    fb_mask = bit0 || bit1;
#endif
}

void Draw_ClearImage(RECT* rect, u_char r, u_char g, u_char b) {
    (void)rect;
    (void)r;
    (void)g;
    (void)b;
    fail("Draw_ClearImage");
}

// texture store: raw halfwords as uploaded,
// keyed by VRAM rect, limits 2,048 B and 8 rects. An identical rect is
// overwritten in place. An upload past a limit logs, counts load_failed
// (psyz_saturn_stats[3], cumulative) and stores nothing; it never aborts.
#define TEX_STORE_BYTES 2048
#define TEX_STORE_RECTS 8
static uint16_t tex_store[TEX_STORE_BYTES / 2];
static struct TexRect {
    RECT rect;
    unsigned off; // halfwords into tex_store
} tex_rects[TEX_STORE_RECTS];
static unsigned tex_nrects, tex_used; // tex_used in halfwords
#ifndef CART_LOAD // a host build of this block alone: no cart
#define CART_LOAD(rect, p) 0
#endif
#ifndef TEX_DIRTY_LOAD // the same cut: no texture cache
#define TEX_DIRTY_LOAD(rect)
#endif
void Draw_LoadImage(RECT* rect, u_long* p) {
    unsigned i, n;
    TEX_DIRTY_LOAD(rect);     // both paths, before the write
    if (CART_LOAD(rect, p)) { // the cart shadow, else the store
        return;
    }
    for (i = 0; i < tex_nrects; i++) {
        RECT* r = &tex_rects[i].rect;
        if (r->x == rect->x && r->y == rect->y && r->w == rect->w &&
            r->h == rect->h) {
            memcpy(tex_store + tex_rects[i].off, p, (size_t)r->w * r->h * 2);
            return;
        }
    }
    n = rect->w > 0 && rect->h > 0 ? (unsigned)(rect->w * rect->h) : 0;
    if (!n || n > TEX_STORE_BYTES / 2 - tex_used ||
        tex_nrects >= TEX_STORE_RECTS) {
        ERRORF("LoadImage rect {%d,%d,%d,%d} past the texture store: "
               "%u of %d B, %u of %d rects used",
               rect->x, rect->y, rect->w, rect->h, tex_used * 2,
               TEX_STORE_BYTES, tex_nrects, TEX_STORE_RECTS);
        psyz_saturn_stats[3]++;
        return;
    }
    tex_rects[tex_nrects].rect = *rect;
    tex_rects[tex_nrects++].off = tex_used;
    memcpy(tex_store + tex_used, p, (size_t)n * 2);
    tex_used += n;
}

// cart path: read the shadow as LoadImage writes it (the odd pad halfword is
// never written); a rect meeting the 320x240 frame counts store_fb, as the
// shadow holds uploads only. No cart: counted (store_nocart), p untouched.
void Draw_StoreImage(RECT* rect, u_long* p) {
    uint16_t* d = (uint16_t*)p;
    int w, h, i, j;
    cart_detect();
    if (!shadow) {
        psyz_saturn_stats[9]++;
        return;
    }
    cart_clamp(rect, &w, &h);
    if (w && h && rect->x < 320 && rect->x + w > 0 && rect->y < 240 &&
        rect->y + h > 0) {
        psyz_saturn_stats[11]++;
    }
#if PSYZ_SATURN_FB_FEEDBACK
    if (fb_pending(rect->x, rect->y, w, h)) {
        FB(PENDING)++;
    }
#endif
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            d[j * w + i] = SHADOW_R16(SHADOW_AD(rect->x + i, rect->y + j));
        }
    }
}

// cart path: GPU 80h sizes ((w-1)&0x3FF)+1, ((h-1)&0x1FF)+1; rows ascending,
// each in 128-halfword chunks through a WRAM buffer; x and y wrap per
// halfword. No cart: counted (move_nocart), nothing moved.
void Draw_MoveImage(RECT* rect, unsigned int x, unsigned int y) {
    static uint16_t tmp[128];
    int w = rect->w, h = rect->h, i, j, c, n;
    cart_detect();
    if (!shadow) {
        psyz_saturn_stats[10]++;
        return;
    }
    if (!w || !h) {
        return;
    }
    w = ((w - 1) & 0x3FF) + 1;
    h = ((h - 1) & 0x1FF) + 1;
#if PSYZ_SATURN_FB_FEEDBACK
    if (fb_pending(rect->x, rect->y, w, h)) {
        FB(PENDING)++;
    }
    if (fb_meets(x, y, w, h, 0, 0, 320, 240)) {
        FB(MOVE_IN)++;
    }
#endif
    tex_dirty(x, y, w, h);
    for (j = 0; j < h; j++) {
        for (c = 0; c < w; c += 128) {
            n = w - c < 128 ? w - c : 128;
            for (i = 0; i < n; i++) {
                tmp[i] = SHADOW_R16(SHADOW_AD(rect->x + c + i, rect->y + j));
            }
            for (i = 0; i < n; i++) {
                SHADOW_W16(SHADOW_AD(x + c + i, y + j), tmp[i]);
            }
        }
    }
}

void Draw_ResetBuffer(void) {
    npoly = 0;
    unsupported = 0;
#if PSYZ_SATURN_DCPU
    ndesc = 0;
#endif
#if PSYZ_SATURN_FB_FEEDBACK
    fb_n = 0;
    fb_void = 0;
#endif
    frame_start = tick();
}

void Draw_FlushBuffer(void) {
    ensure_init();
#if PSYZ_SATURN_DCPU
    dcpu_flush();
#endif
    if (npoly || unsupported) {
        commit();
    }
}

int Draw_ExequeSync(void) { return 0; } // commit() already waited

// texture cache: each character is the
// sprite's texels converted to RGB 32768 (`0x8000 | t` for t != 0, else the
// transparent 0x0000), width padded to a multiple of 8 with 0x0000, start
// 16-byte aligned (Mednafen's RGB fetch) and preceded by a 0x0000 guard
// halfword. Keys persist across frames: a key whose
// texel or CLUT footprint meets a LoadImage or MoveImage destination is dead
// (w 0), its arena bytes kept until the next eviction. A size VDP1 cannot take
// is counted unsupported.
static struct TexKey {
    unsigned tx, ty, tpf, clut, u, v, w, h;
    unsigned rx, ry, rw, rh; // texel footprint, PS1 VRAM halfwords
    unsigned cx, cy, cw;     // CLUT footprint (cw 0: none)
    unsigned n;              // converted bytes, 0 for a free slot
#if PSYZ_SATURN_TEX_LRU
    unsigned use; // tex_clock at the last hit or insert
    unsigned off; // block start: 16 bytes, guard at off + 14, then n bytes
#endif
#if PSYZ_SATURN_FB_FEEDBACK
    int hole, stp; // a drawn texel is 0, has bit 15
#endif
    uint16_t* p;
} tex_keys[MAXPOLY];

// one axis of the overlap test, modulo m (no wrap splitting)
static int tex_ov(
    unsigned a, unsigned an, unsigned b, unsigned bn, unsigned m) {
    return ((b - a) & (m - 1)) < an || ((a - b) & (m - 1)) < bn;
}

// a dirty span: every live key whose footprint meets it dies
static void tex_dirty(int x, int y, int w, int h) {
    unsigned i;
    CS(DIRTY_SPANS)++;
    for (i = 0; i < tex_nkeys; i++) {
        struct TexKey* k = &tex_keys[i];
        if (k->w && ((tex_ov(k->rx, k->rw, x, w, 1024) &&
                      tex_ov(k->ry, k->rh, y, h, 512)) ||
                     (k->cw && tex_ov(k->cx, k->cw, x, w, 1024) &&
                      tex_ov(k->cy, 1, y, h, 512)))) {
            k->w = 0;
            CS(INVALIDATIONS)++;
            CS(LIVE)--;
        }
    }
}

// bytes of a reclaimed key by cause: dead, else space or refusal
static void tex_count_ev(const struct TexKey* k, int refusal) {
    unsigned c = !k->w ? TS_EV_INVAL : refusal ? TS_EV_REFUSAL : TS_EV_CAP;
    psyz_tex_stats[c]++;
    psyz_tex_stats[c + 1] += k->n;
}

#if PSYZ_SATURN_TEX_LRU
// lowest gap of at least need bytes between held blocks, TEX_CACHE_BYTES if
// none; *big the largest gap, *sum all free bytes. O(keys^2), keys <= 64
static unsigned tex_gap(unsigned need, unsigned* big, unsigned* sum) {
    unsigned at = 0, fit = TEX_CACHE_BYTES, i, next;
    *big = *sum = 0;
    for (;;) {
        struct TexKey* b = NULL;
        for (i = 0; i < tex_nkeys; i++) {
            struct TexKey* k = &tex_keys[i];
            if (k->n && k->off >= at && (!b || k->off < b->off)) {
                b = k;
            }
        }
        next = b ? b->off : TEX_CACHE_BYTES;
        if (next - at >= need && fit == TEX_CACHE_BYTES) {
            fit = at;
        }
        if (next - at > *big) {
            *big = next - at;
        }
        *sum += next - at;
        if (!b) {
            return fit;
        }
        at = b->off + 16 + b->n;
    }
}
#else
// drop-all, LRU or a mid-list commit if churn shows it
static void tex_drop(int refusal) {
    unsigned i;
    for (i = 0; i < tex_nkeys; i++) {
        tex_count_ev(&tex_keys[i], refusal);
    }
    tex_nkeys = 0;
    tex_arena_used = 0;
    tex_evict_pending = 0;
    tex_half ^= tex_half == tex_plot_half ? TEX_CACHE_BYTES / 2 : 0;
    CS(EVICTIONS)++;
    CS(LIVE) = 0;
}
#endif

// PS1 VRAM halfword from the cart shadow, else from the texture store, where
// outside every rect reads 0x0000
static uint16_t tex_hw(int x, int y) {
    unsigned i;
    if (shadow) {
        return SHADOW_R16(SHADOW_AD(x, y));
    }
    for (i = 0; i < tex_nrects; i++) {
        RECT* r = &tex_rects[i].rect;
        int dx = x - r->x, dy = y - r->y;
        if (dx >= 0 && dx < r->w && dy >= 0 && dy < r->h) {
            return tex_store[tex_rects[i].off + dy * r->w + dx];
        }
    }
    return 0x0000;
}

static uint16_t texel(unsigned clut, unsigned u, unsigned v) {
    int px = texpage.tx * 64, py = texpage.ty * 256;
    uint16_t t;
    if (texpage.tpf == 0) { // 4-bit: low nibble first, then the CLUT
        unsigned idx = (tex_hw(px + u / 4, py + v) >> (4 * (u & 3))) & 15;
        t = tex_hw((clut & 0x3F) * 16 + idx, (clut >> 6) & 0x1FF);
    } else if (texpage.tpf == 1) { // 8-bit: low byte first, 256-entry CLUT
        unsigned idx = (tex_hw(px + u / 2, py + v) >> (8 * (u & 1))) & 0xFF;
        t = tex_hw((clut & 0x3F) * 16 + idx, (clut >> 6) & 0x1FF);
    } else {
        t = tex_hw(px + u, py + v);
    }
#if PSYZ_SATURN_FB_FEEDBACK
    fb_hole |= !t;
    fb_stp |= t >> 15;
#endif
    return t ? 0x8000 | (t & 0x7FFF) : 0x0000;
}

static void sprite(int x, int y, unsigned w, unsigned h, unsigned w2) {
    unsigned clut = w2 >> 16, u = w2 & 0xFF, v = (w2 >> 8) & 0xFF;
    unsigned wpad = (w + 7) & ~7u, i, j, start, tpw = 4 >> texpage.tpf;
    uint32_t t0, t;
    uint16_t* p = NULL;
    vdp1_cmdt_t* c;
    vdp1_cmdt_draw_mode_t dm = {.raw = 0};
#if PSYZ_SATURN_FB_FEEDBACK
    int hole = 0, stp = 0;
#endif
    if (npoly >= MAXPOLY || texpage.tpf > 2 || wpad < 8 || wpad > 504 ||
        h < 1 || h > 255) {
        unsupported++;
        return;
    }
    CS(LOOKUPS)++;
#if PSYZ_SATURN_TEX_LRU
    tex_clock++;
#endif
    for (i = 0; i < tex_nkeys && !p; i++) {
        struct TexKey* k = &tex_keys[i];
        if (k->tx == texpage.tx && k->ty == texpage.ty &&
            k->tpf == texpage.tpf && k->clut == clut && k->u == u &&
            k->v == v && k->w == w && k->h == h) {
            p = k->p;
#if PSYZ_SATURN_TEX_LRU
            k->use = tex_clock;
#endif
#if PSYZ_SATURN_FB_FEEDBACK
            hole = k->hole;
            stp = k->stp;
#endif
        }
    }
    if (p) {
        CS(HITS)++;
    } else {
#if PSYZ_SATURN_TEX_LRU
        // first fit, else reclaim the oldest unpinned key (dead ones first)
        // and retry; a key used since the last commit is pinned: pending
        // commands read its bytes. Wrap-safe for < 2^31 lookups a commit
        unsigned need = 16 + wpad * h * 2, big, sum, slot, pinned;
        for (;;) {
            struct TexKey* o = NULL;
            start = tex_gap(need, &big, &sum);
            for (slot = 0; slot < tex_nkeys && tex_keys[slot].n; slot++) {
            }
            if (start < TEX_CACHE_BYTES && slot < MAXPOLY) {
                break;
            }
            for (i = pinned = 0; i < tex_nkeys; i++) {
                struct TexKey* k = &tex_keys[i];
                if (!k->n) {
                    continue;
                }
                if ((int)(k->use - tex_pin) > 0) {
                    pinned++;
                } else if (!o || (!k->w && o->w) ||
                           (!k->w == !o->w && (int)(k->use - o->use) < 0)) {
                    o = k;
                }
            }
            if (!o) {
                CS(FULL)++;
                TS(REFUSALS)++;
                TS(SLOT_REFUSALS) += slot >= MAXPOLY;
                TS(FRAG_REFUSALS) += start == TEX_CACHE_BYTES && sum >= need;
                if (pinned > TS(PINNED_AT_REFUSAL)) {
                    TS(PINNED_AT_REFUSAL) = pinned;
                }
                unsupported++;
                return;
            }
            tex_count_ev(o, 0);
            if (o->w) {
                CS(LIVE)--;
            }
            o->w = 0;
            o->n = 0;
            CS(EVICTIONS)++;
        }
        start += 16;
#else
        start = (tex_arena_used + 2 + 15) & ~15u;
        if (16 + wpad * h * 2 > TEX_CACHE_BYTES / 2) {
            CS(FULL)++;
            TS(REFUSALS)++;
            unsupported++;
            return;
        }
        if (tex_nkeys >= MAXPOLY ||
            start + wpad * h * 2 > TEX_CACHE_BYTES / 2) {
            if (npoly) { // pending commands read the arena: drop at commit
                CS(FULL)++;
                TS(REFUSALS)++;
                TS(SLOT_REFUSALS) += tex_nkeys >= MAXPOLY;
                if (tex_nkeys > TS(PINNED_AT_REFUSAL)) {
                    TS(PINNED_AT_REFUSAL) = tex_nkeys;
                }
                tex_evict_pending = 1;
                unsupported++;
                return;
            }
            tex_drop(0);
            start = 16;
        }
        start += tex_half;
#endif
        p = tex_arena + start / 2;
        p[-1] = 0x0000; // guard
        t0 = t = tick();
#if PSYZ_SATURN_FB_FEEDBACK
        fb_hole = fb_stp = 0;
#endif
        for (j = 0; j < h; j++) {
            for (i = 0; i < wpad; i++) {
                p[j * wpad + i] =
                    i < w ? texel(clut, (u + i) & 0xFF, (v + j) & 0xFF) : 0;
            }
            t = tick(); // once per row
        }
        CS(COPY_TICKS) += t - t0;
        if (shadow) {
            CS(BYTES_CART) += wpad * h * 2;
        } else {
            CS(BYTES_STORE) += wpad * h * 2;
        }
        TS(UPLOADS)++;
        TS(UP_BYTES) += wpad * h * 2;
#if PSYZ_SATURN_TEX_LRU
        if (start + wpad * h * 2 > CS(ARENA_HIGH)) {
            CS(ARENA_HIGH) = start + wpad * h * 2;
        }
#else
        tex_arena_used = start - tex_half + wpad * h * 2;
        if (tex_arena_used > CS(ARENA_HIGH)) {
            CS(ARENA_HIGH) = tex_arena_used;
        }
#endif
        {
            // footprint: texel rect (x0, halfword width, rows), the whole
            // page width or height when u or v wraps; CLUT rect for tpf 0, 1
            struct TexKey k = {texpage.tx, texpage.ty, texpage.tpf, clut, u,
                               v,          w,          h,           0};
            k.rx = texpage.tx * 64 + u / tpw;
            k.rw = (u + w - 1) / tpw - u / tpw + 1;
            k.ry = texpage.ty * 256 + v;
            k.rh = h;
            // a wrapping sprite claims the whole page axis
            if (u + w > 256) {
                k.rx = texpage.tx * 64;
                k.rw = 256 / tpw;
            }
            if (v + h > 256) {
                k.ry = texpage.ty * 256;
                k.rh = 256;
            }
            k.cx = (clut & 0x3F) * 16;
            k.cy = (clut >> 6) & 0x1FF;
            k.cw = texpage.tpf == 2 ? 0 : texpage.tpf ? 256 : 16;
            k.n = wpad * h * 2;
            k.p = p;
#if PSYZ_SATURN_FB_FEEDBACK
            k.hole = hole = fb_hole;
            k.stp = stp = fb_stp;
#endif
#if PSYZ_SATURN_TEX_LRU
            k.use = tex_clock;
            k.off = start - 16;
            tex_keys[slot] = k;
            tex_nkeys += slot == tex_nkeys;
#else
            tex_keys[tex_nkeys++] = k;
#endif
            CS(LIVE)++;
        }
    }
    c = &list->cmdts[2 + npoly++];
    vdp1_cmdt_normal_sprite_set(c);
    dm.end_code_disable = 1;
    dm.color_mode = VDP1_CMDT_CM_RGB_32768; // CMDPMOD 0x00A8, SPD 0
    vdp1_cmdt_draw_mode_set(c, dm);
    vdp1_cmdt_char_base_set(c, (vdp1_vram_t)p);
    vdp1_cmdt_char_size_set(c, wpad, h);
    c->cmd_xa = x;
    c->cmd_ya = y;
#if PSYZ_SATURN_FB_FEEDBACK
    fb_void |= stp;
    if (!hole) {
        fb_note(x, y, w, h);
    }
#endif
}

int Draw_PushPrim(u_long* packets, int max_len) {
    unsigned w0 = (unsigned)(packets[0] & 0xFFFFFFFFu);
    unsigned code = w0 >> 24;
    int words;
    ensure_init();
    if (code >= 0x60 && code <= 0x7F) {
        // TILE, SPRT, TILE_1, SPRT_1, TILE_8, SPRT_8, TILE_16, SPRT_16
        static const int len[8] = {3, 4, 2, 3, 2, 3, 2, 3};
        words = len[(code >> 2) & 7];
    } else if (code >= 0x28 && code <= 0x2B) {
        words = 5;
    } else if (code >= 0x2C && code <= 0x2F) {
        unsupported++;
        return 9;
    } else {
        ERRORF("Draw_PushPrim code %02X", code);
        abort();
    }
    if (max_len < words) {
        ERRORF("Draw_PushPrim code %02X needs %d words, got %d", code, words,
               max_len);
        abort();
    }
    if (code & 2) { // semi-transparent: no VDP1 mapping
        unsupported++;
        return words;
    }
#if PSYZ_SATURN_DCPU
    if (dcpu_on && code <= 0x63 && dcpu_queue(packets, code)) { // TILE, F4
        return words;
    }
#endif
    if (code >= 0x64) {
        static const unsigned side[4] = {0, 1, 8, 16}; // by code bits 4-3
        unsigned w1 = (unsigned)(packets[1] & 0xFFFFFFFFu);
        unsigned s = side[(code >> 3) & 3];
        int x = (short)(w1 & 0xFFFF), y = (short)(w1 >> 16);
        if (!(code & 4) && !(code & 1)) { // TILE_1/8/16 0x68, 0x70, 0x78
            tile(w0, x, y, s, s);
        } else if ((code & 4) && ((code & 1) || (w0 & 0xFFFFFF) == 0x808080)) {
            unsigned w3 =
                s ? s << 16 | s : (unsigned)(packets[3] & 0xFFFFFFFFu);
            sprite(x, y, w3 & 0xFFFF, w3 >> 16,
                   (unsigned)(packets[2] & 0xFFFFFFFFu));
        } else {
            unsupported++; // TILE_n bit 0, tinted sprite
        }
        return words;
    }
    if (code >= 0x60) {
        unsigned w1 = (unsigned)(packets[1] & 0xFFFFFFFFu);
        unsigned w2 = (unsigned)(packets[2] & 0xFFFFFFFFu);
        int x = (short)(w1 & 0xFFFF), y = (short)(w1 >> 16);
        int w = (short)(w2 & 0xFFFF), h = (short)(w2 >> 16);
        if (w > 0 && h > 0) {
            tile(w0, x, y, w, h);
        } else {
            unsupported++;
        }
    } else {
        int16_t x[4], y[4], mx, my;
        int16_vec2_t v[4];
        int i;
        // PS1 vertex order v0 v1 v2 v3 is a Z; the VDP1 ring is v0 v1 v3 v2
        static const int ring[4] = {1, 2, 4, 3};
        for (i = 0; i < 4; i++) {
            unsigned w = (unsigned)(packets[ring[i]] & 0xFFFFFFFFu);
            x[i] = (short)(w & 0xFFFF);
            y[i] = (short)(w >> 16);
        }
        mx = x[0];
        my = y[0];
        for (i = 1; i < 4; i++) {
            if (x[i] > mx) {
                mx = x[i];
            }
            if (y[i] > my) {
                my = y[i];
            }
        }
        // max-x/max-y vertices -1 is exact only for axis-aligned
        // quads (PS1 excludes the right and bottom edges)
        for (i = 0; i < 4; i++) {
            v[i].x = x[i] == mx ? x[i] - 1 : x[i];
            v[i].y = y[i] == my ? y[i] - 1 : y[i];
        }
        poly(w0, v);
    }
    return words;
}

// the backend's main WRAM statics (MoveImage's 256 B buffer and scalars not
// counted)
static unsigned wram_bytes(void) {
    return sizeof psyz_saturn_stats + sizeof psyz_cart_stats +
           sizeof tex_store + sizeof tex_rects + sizeof tex_keys +
           sizeof texpage;
}
