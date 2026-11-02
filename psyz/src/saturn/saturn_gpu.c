#include <yaul.h>
#include <psyz.h>
#include <libgpu.h>
#include <psyz/log.h>
#include "draw.h"
#include <stdlib.h>
#include <string.h>

#define MAXPOLY 64

static vdp1_cmdt_list_t* list;
static unsigned npoly, unsupported;
static int16_vec2_t sysclip = INT16_VEC2_INITIALIZER(319, 239);
static int16_vec2_t local;
static uint16_t* tex_arena;
static unsigned tex_arena_used;
static unsigned tex_evict_pending;
#define TEX_CACHE_BYTES 262144
static unsigned tex_nkeys;
static void tex_drop(void);

static void fail(const char* fn) {
    ERRORF("%s is not implemented on Saturn", fn);
    abort();
}

#define SHADOW_R8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define SHADOW_R16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define SHADOW_W16(a, v) (*(volatile uint16_t*)(uintptr_t)(a) = (uint16_t)(v))
#define SHADOW_BASE 0x22400000UL
#define SHADOW_AD(x, y)                                                        \
    (SHADOW_BASE +                                                             \
     ((((uint32_t)(y) & 0x1FF) << 10 | ((uint32_t)(x) & 0x3FF)) << 1))
#define CART_ID_ADDR 0x24FFFFFFUL
#define CART_UNIT 0x80000UL
static int cart_done, shadow;

// libyaul's cart size is not trusted (aliased carts misreport): every 512 KiB
// unit must read back distinct
static void cart_probe(void) {
    unsigned u, id = 0, units = 0;
    uint32_t a;
    dram_cart_init();
    if (dram_cart_id_get() != DRAM_CART_ID_INVALID) {
        id = SHADOW_R8(CART_ID_ADDR);
    }
    if (id != DRAM_CART_ID_4MIB) {
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
    if (units != 8) {
        return;
    }
    for (a = 0; a < 0x100000; a += 2) {
        SHADOW_W16(SHADOW_BASE + a, 0);
    }
    shadow = 1;
}
static void cart_detect(void) {
    if (cart_done) {
        return;
    }
    cart_done = 1;
    cart_probe();
}

static void cart_clamp(const RECT* r, int* w, int* h) {
    *w = r->w < 0 ? 0 : r->w > 1024 ? 1024 : r->w;
    *h = r->h < 0 ? 0 : r->h > 512 ? 512 : r->h;
}

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

static void tex_dirty(int x, int y, int w, int h);
static void tex_dirty_load(const RECT* r) {
    int w, h;
    cart_clamp(r, &w, &h);
    if (w * h > 0) {
        tex_dirty(r->x, r->y, w, h);
    }
}

static void ensure_init(void) {
    vdp1_env_t env;
    vdp1_vram_partitions_t vp;
    cart_detect();
    if (list) {
        return;
    }
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
        unsupported++;
        return;
    }
    c = &list->cmdts[2 + npoly++];
    vdp1_cmdt_polygon_set(c);
    dm.pre_clipping_disable = 1;
    vdp1_cmdt_draw_mode_set(c, dm);
    vdp1_cmdt_color_set(c, color(w0));
    vdp1_cmdt_vtx_set(c, v);
}

static void tile(unsigned w0, int x, int y, int w, int h) {
    int16_vec2_t v[4] = {
        {x, y}, {x + w - 1, y}, {x + w - 1, y + h - 1}, {x, y + h - 1}};
    poly(w0, v);
}

static void commit(void) {
    vdp1_cmdt_t* c = list->cmdts;
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
    npoly = 0;
    unsupported = 0;
    if (tex_evict_pending) {
        tex_drop();
    }
    if (tex_nkeys > MAXPOLY / 2 || tex_arena_used > TEX_CACHE_BYTES / 2) {
        tex_drop();
    }
}

void Draw_Reset(void) { fail("Draw_Reset"); }

void Draw_DisplayEnable(unsigned int on) {
    if (on) {
        return;
    }
    fail("Draw_DisplayEnable");
}

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

// draw.h's bit-fields assume LSB-first allocation; SH GCC allocates MSB-first
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
    vdp2_sync();
    vdp2_sync_wait();
}

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

void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {
    (void)mask_x;
    (void)mask_y;
    (void)off_x;
    (void)off_y;
}

void Draw_SetAreaStart(int x, int y) {
    (void)x;
    (void)y;
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
}

void Draw_ClearImage(RECT* rect, u_char r, u_char g, u_char b) {
    (void)rect;
    (void)r;
    (void)g;
    (void)b;
    fail("Draw_ClearImage");
}

#define TEX_STORE_BYTES 2048
#define TEX_STORE_RECTS 8
static uint16_t tex_store[TEX_STORE_BYTES / 2];
static struct TexRect {
    RECT rect;
    unsigned off;
} tex_rects[TEX_STORE_RECTS];
static unsigned tex_nrects, tex_used;
void Draw_LoadImage(RECT* rect, u_long* p) {
    unsigned i, n;
    tex_dirty_load(rect);
    if (cart_load(rect, p)) {
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
        return;
    }
    tex_rects[tex_nrects].rect = *rect;
    tex_rects[tex_nrects++].off = tex_used;
    memcpy(tex_store + tex_used, p, (size_t)n * 2);
    tex_used += n;
}

void Draw_StoreImage(RECT* rect, u_long* p) {
    uint16_t* d = (uint16_t*)p;
    int w, h, i, j;
    cart_detect();
    if (!shadow) {
        return;
    }
    cart_clamp(rect, &w, &h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            d[j * w + i] = SHADOW_R16(SHADOW_AD(rect->x + i, rect->y + j));
        }
    }
}

void Draw_MoveImage(RECT* rect, unsigned int x, unsigned int y) {
    static uint16_t tmp[128];
    int w = rect->w, h = rect->h, i, j, c, n;
    cart_detect();
    if (!shadow) {
        return;
    }
    if (!w || !h) {
        return;
    }
    w = ((w - 1) & 0x3FF) + 1;
    h = ((h - 1) & 0x1FF) + 1;
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
}

void Draw_FlushBuffer(void) {
    ensure_init();
    if (npoly || unsupported) {
        commit();
    }
}

int Draw_ExequeSync(void) { return 0; }

static struct TexKey {
    unsigned tx, ty, tpf, clut, u, v, w, h;
    unsigned rx, ry, rw, rh;
    unsigned cx, cy, cw;
    uint16_t* p;
} tex_keys[MAXPOLY];

static int tex_ov(
    unsigned a, unsigned an, unsigned b, unsigned bn, unsigned m) {
    return ((b - a) & (m - 1)) < an || ((a - b) & (m - 1)) < bn;
}

static void tex_dirty(int x, int y, int w, int h) {
    unsigned i;
    for (i = 0; i < tex_nkeys; i++) {
        struct TexKey* k = &tex_keys[i];
        if (k->w && ((tex_ov(k->rx, k->rw, x, w, 1024) &&
                      tex_ov(k->ry, k->rh, y, h, 512)) ||
                     (k->cw && tex_ov(k->cx, k->cw, x, w, 1024) &&
                      tex_ov(k->cy, 1, y, h, 512)))) {
            k->w = 0;
        }
    }
}

static void tex_drop(void) {
    tex_nkeys = 0;
    tex_arena_used = 0;
    tex_evict_pending = 0;
}

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
    if (texpage.tpf == 0) {
        unsigned idx = (tex_hw(px + u / 4, py + v) >> (4 * (u & 3))) & 15;
        t = tex_hw((clut & 0x3F) * 16 + idx, (clut >> 6) & 0x1FF);
    } else if (texpage.tpf == 1) {
        unsigned idx = (tex_hw(px + u / 2, py + v) >> (8 * (u & 1))) & 0xFF;
        t = tex_hw((clut & 0x3F) * 16 + idx, (clut >> 6) & 0x1FF);
    } else {
        t = tex_hw(px + u, py + v);
    }
    return t ? 0x8000 | (t & 0x7FFF) : 0x0000;
}

static void sprite(int x, int y, unsigned w, unsigned h, unsigned w2) {
    unsigned clut = w2 >> 16, u = w2 & 0xFF, v = (w2 >> 8) & 0xFF;
    unsigned wpad = (w + 7) & ~7u, i, j, start, tpw = 4 >> texpage.tpf;
    uint16_t* p = NULL;
    vdp1_cmdt_t* c;
    vdp1_cmdt_draw_mode_t dm = {.raw = 0};
    if (npoly >= MAXPOLY || texpage.tpf > 2 || wpad < 8 || wpad > 504 ||
        h < 1 || h > 255) {
        unsupported++;
        return;
    }
    for (i = 0; i < tex_nkeys && !p; i++) {
        struct TexKey* k = &tex_keys[i];
        if (k->tx == texpage.tx && k->ty == texpage.ty &&
            k->tpf == texpage.tpf && k->clut == clut && k->u == u &&
            k->v == v && k->w == w && k->h == h) {
            p = k->p;
        }
    }
    if (!p) {
        start = (tex_arena_used + 2 + 15) & ~15u;
        if (tex_nkeys >= MAXPOLY || start + wpad * h * 2 > TEX_CACHE_BYTES) {
            if (npoly) {
                tex_evict_pending = 1;
                unsupported++;
                return;
            }
            tex_drop();
            start = 16;
        }
        p = tex_arena + start / 2;
        p[-1] = 0x0000;
        for (j = 0; j < h; j++) {
            for (i = 0; i < wpad; i++) {
                p[j * wpad + i] =
                    i < w ? texel(clut, (u + i) & 0xFF, (v + j) & 0xFF) : 0;
            }
        }
        tex_arena_used = start + wpad * h * 2;
        {
            struct TexKey k = {texpage.tx, texpage.ty, texpage.tpf, clut, u,
                               v,          w,          h,           0};
            k.rx = texpage.tx * 64 + u / tpw;
            k.rw = (u + w - 1) / tpw - u / tpw + 1;
            k.ry = texpage.ty * 256 + v;
            k.rh = h;
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
            k.p = p;
            tex_keys[tex_nkeys++] = k;
        }
    }
    c = &list->cmdts[2 + npoly++];
    vdp1_cmdt_normal_sprite_set(c);
    dm.end_code_disable = 1;
    dm.color_mode = VDP1_CMDT_CM_RGB_32768;
    vdp1_cmdt_draw_mode_set(c, dm);
    vdp1_cmdt_char_base_set(c, (vdp1_vram_t)p);
    vdp1_cmdt_char_size_set(c, wpad, h);
    c->cmd_xa = x;
    c->cmd_ya = y;
}

int Draw_PushPrim(u_long* packets, int max_len) {
    unsigned w0 = (unsigned)(packets[0] & 0xFFFFFFFFu);
    unsigned code = w0 >> 24;
    int words;
    ensure_init();
    if (code >= 0x60 && code <= 0x7F) {
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
    if (code & 2) {
        unsupported++;
        return words;
    }
    if (code >= 0x64) {
        static const unsigned side[4] = {0, 1, 8, 16};
        unsigned w1 = (unsigned)(packets[1] & 0xFFFFFFFFu);
        unsigned s = side[(code >> 3) & 3];
        int x = (short)(w1 & 0xFFFF), y = (short)(w1 >> 16);
        if (!(code & 4) && !(code & 1)) {
            tile(w0, x, y, s, s);
        } else if ((code & 4) && ((code & 1) || (w0 & 0xFFFFFF) == 0x808080)) {
            unsigned w3 =
                s ? s << 16 | s : (unsigned)(packets[3] & 0xFFFFFFFFu);
            sprite(x, y, w3 & 0xFFFF, w3 >> 16,
                   (unsigned)(packets[2] & 0xFFFFFFFFu));
        } else {
            unsupported++;
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
        for (i = 0; i < 4; i++) {
            v[i].x = x[i] == mx ? x[i] - 1 : x[i];
            v[i].y = y[i] == my ? y[i] - 1 : y[i];
        }
        poly(w0, v);
    }
    return words;
}
