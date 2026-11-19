#include "saturn_pad.h"
#include <string.h>

#define SMPC_IREG(n) (0x20100001u + 2u * (n))
#define SMPC_COMREG 0x2010001Fu
#define SMPC_OREG(n) (0x20100021u + 2u * (n))
#define SMPC_SR 0x20100061u
#define SMPC_SF 0x20100063u

#define CMD_INTBACK 0x10u
#define IREG1_PEN 0x08u
#define IREG1_OPE 0x02u
#define IREG0_BREAK 0x40u
#define SR_NPE 0x20u

#define LINES_300US 5u
#define SF_POLL_BOUND 20000u
#define VCNT_SPIN_BOUND 20000u

#define RD8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define WR8(a, v) (*(volatile uint8_t*)(uintptr_t)(a) = (uint8_t)(v))

#define PADLup (1u << 12)
#define PADLdown (1u << 14)
#define PADLleft (1u << 15)
#define PADLright (1u << 13)
#define PADRup (1u << 4)
#define PADRdown (1u << 6)
#define PADRleft (1u << 7)
#define PADRright (1u << 5)
#define PADselect (1u << 8)
#define PADstart (1u << 11)
#define PADL1 (1u << 2)
#define PADL2 (1u << 0)
#define PADR1 (1u << 3)
#define PADR2 (1u << 1)
#define KIND_DIGITAL 0x41u
#define KIND_ANALOG 0x73u

static const uint16_t map[13][2] = {
    {SAT_UP, PADLup},       {SAT_DOWN, PADLdown},  {SAT_LEFT, PADLleft},
    {SAT_RIGHT, PADLright}, {SAT_START, PADstart}, {SAT_A, PADRdown},
    {SAT_B, PADRright},     {SAT_X, PADRleft},     {SAT_Y, PADRup},
    {SAT_L, PADL1},         {SAT_R, PADL2},        {SAT_Z, PADR1},
    {SAT_C, PADR2},
};

static saturn_pad_cfg_t cfg;
static uint8_t frames[2][8];
static int pending;

void saturn_pad_init(const saturn_pad_cfg_t* c) {
    static const saturn_pad_cfg_t def = {1, 0, 0, 0, 224};
    cfg = c ? *c : def;
    memset(frames, 0xFF, sizeof frames);
    pending = 0;
}

static int sf_wait(void) {
    unsigned n;
    for (n = 0; n < SF_POLL_BOUND; n++)
        if (!(RD8(SMPC_SF) & 1u))
            return 0;
    return -1;
}

static void build(int idx, uint8_t id, const uint8_t* d) {
    uint8_t* f = frames[idx];
    uint16_t w = (uint16_t)(((~d[0] & 0xFFu) << 8) | (~d[1] & 0xFFu)), p = 0;
    unsigned i;
    if (cfg.select_chord && (w & cfg.select_chord) == cfg.select_chord) {
        w = (uint16_t)(w & ~cfg.select_chord);
        p |= PADselect;
    }
    for (i = 0; i < 13; i++)
        if (w & map[i][0])
            p = (uint16_t)(p | map[i][1]);
    memset(f, 0, 8);
    f[2] = (uint8_t)~(p >> 8);
    f[3] = (uint8_t)~p;
    if (id == 0x16) {
        f[1] = KIND_ANALOG;
        f[4] = f[5] = 0x80;
        f[6] = (uint8_t)(cfg.axis_x_flip ? d[2] ^ 0xFFu : d[2]);
        f[7] = (uint8_t)(cfg.axis_y_flip ? d[3] ^ 0xFFu : d[3]);
    } else {
        f[1] = KIND_DIGITAL;
    }
}

static void saturn_pad_decode(const uint8_t* oreg, unsigned len) {
    unsigned i = 0, port, k;
    for (port = 0; port < 2; port++) {
        uint8_t st, id, size;
        int idx = (int)port;
        if (i >= len)
            return;
        st = oreg[i++];
        if ((st & 0x0Fu) == 0) {
            memset(frames[idx], 0xFF, 8);
            continue;
        }
        for (k = 0; k < (st & 0x0Fu); k++) {
            if (i >= len)
                return;
            id = oreg[i++];
            size = id == 0xFFu ? 0 : (uint8_t)(id & 0x0Fu);
            if (size == 0x0Fu || i + size > len)
                return;
            if ((st & 0xF0u) == 0xF0u && (id == 0x02 || id == 0x16))
                build(idx, id, &oreg[i]);
            i += size;
        }
        if ((st & 0xF0u) != 0xF0u)
            memset(frames[idx], 0xFF, 8);
    }
}

static void saturn_pad_collect(void) {
    uint8_t buf[32];
    unsigned n;
    if (!pending)
        return;
    if (sf_wait())
        return;
    pending = 0;
    for (n = 0; n < 32; n++)
        buf[n] = RD8(SMPC_OREG(n));
    if (RD8(SMPC_SR) & SR_NPE)
        WR8(SMPC_IREG(0), IREG0_BREAK);
    saturn_pad_decode(buf, 32);
}

/* INTBACK may be issued only from V-BLANK-IN + 300 us until V-BLANK-OUT */
static void saturn_pad_issue(void) {
    unsigned v = saturn_frame_vcount(), lo = cfg.vbi_line, n;
    for (n = 0; v >= lo && v < lo + LINES_300US && n < VCNT_SPIN_BOUND; n++)
        v = saturn_frame_vcount();
    if (v < lo + LINES_300US || (RD8(SMPC_SF) & 1u))
        return;
    WR8(SMPC_SF, 1);
    WR8(SMPC_IREG(0), 0x00);
    WR8(SMPC_IREG(1), IREG1_PEN | (cfg.ope ? IREG1_OPE : 0));
    WR8(SMPC_IREG(2), 0xF0);
    WR8(SMPC_COMREG, CMD_INTBACK);
    pending = 1;
}

void saturn_pad_vsync(void) {
    saturn_pad_collect();
    saturn_pad_issue();
}

const uint8_t* saturn_pad_frame(int idx) { return frames[idx & 1]; }
