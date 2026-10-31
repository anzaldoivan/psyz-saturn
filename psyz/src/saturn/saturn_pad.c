/* saturn_pad.c -- sole SMPC owner (see saturn_pad.h for sources).
 * Citations: ST-169 physical pages pNNNN of saturn-docs-3aeb0b9a7788593e
 * (ST-169-R1-072694).
 */
#include "saturn_pad.h"
#include <string.h>

/* Register addresses, p0015 (libyaul smpc/map.h, vdp2/map.h names); byte access
 * only. */
#define SMPC_IREG(n) (0x20100001u + 2u * (n))
#define SMPC_COMREG 0x2010001Fu
#define SMPC_OREG(n) (0x20100021u + 2u * (n))
#define SMPC_SR 0x20100061u
#define SMPC_SF 0x20100063u
#define VDP2_EXTEN 0x25F80002u
#define VDP2_VCNT 0x25F8000Au

#define CMD_SNDON 0x06u   /* p0022 */
#define CMD_SNDOFF 0x07u  /* p0022 */
#define CMD_INTBACK 0x10u /* p0022 */
#define CMD_RESENAB 0x19u /* p0022, p0043 */
#define CMD_RESDISA 0x1Au /* p0022, p0044 */
#define IREG1_PEN 0x08u   /* p0069; P1MD=P2MD=00 15-byte mode */
#define IREG1_OPE 0x02u   /* p0069 */
#define IREG0_BREAK 0x40u /* p0068 */
#define SR_NPE 0x20u      /* p0076 */

#define LINES_300US                                                            \
    5u /* 300 us / 63.6 us (NTSC) or 64 us (PAL) per line, rounded up */
#define SF_POLL_BOUND 20000u /* design value, not a manual value */
#define VCNT_SPIN_BOUND 20000u

#ifdef SATURN_PAD_HOST
uint8_t saturn_pad_hw_rd8(uint32_t a);
void saturn_pad_hw_wr8(uint32_t a, uint8_t v);
uint16_t saturn_pad_hw_rd16(uint32_t a);
#define RD8(a) saturn_pad_hw_rd8(a)
#define WR8(a, v) saturn_pad_hw_wr8((a), (uint8_t)(v))
#define RD16(a) saturn_pad_hw_rd16(a)
#else
#define RD8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define WR8(a, v) (*(volatile uint8_t*)(uintptr_t)(a) = (uint8_t)(v))
#define RD16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#endif

/* PS1 bits, PsyZ libetc.h:25-46 */
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
#define KIND_DIGITAL 0x41u /* psyz/input.h:22-26 */
#define KIND_ANALOG 0x73u

/* Capcom map: A Cross, B Circle, X Square, Y Triangle, L L1, R L2, Z R1, C R2
 */
static const uint16_t map[13][2] = {
    {SAT_UP, PADLup},       {SAT_DOWN, PADLdown},  {SAT_LEFT, PADLleft},
    {SAT_RIGHT, PADLright}, {SAT_START, PADstart}, {SAT_A, PADRdown},
    {SAT_B, PADRright},     {SAT_X, PADRleft},     {SAT_Y, PADRup},
    {SAT_L, PADL1},         {SAT_R, PADL2},        {SAT_Z, PADR1},
    {SAT_C, PADR2},
};

static saturn_pad_cfg_t cfg;
static saturn_pad_stats_t stats;
static uint8_t frames[2][8];
static int pending;

void saturn_pad_init(const saturn_pad_cfg_t* c) {
    static const saturn_pad_cfg_t def = {1, 0, 0, 0, 224};
    cfg = c ? *c : def;
    memset(&stats, 0, sizeof stats);
    memset(frames, 0xFF, sizeof frames); /* psp_input.c:91 disconnected */
    pending = 0;
}

/* bounded wait for SF == 0 (p0016: SMPC resets SF when the command ends) */
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
    memset(f, 0, 8); /* psp_input.c:71 */
    f[2] = (uint8_t)~(p >> 8);
    f[3] = (uint8_t)~p;
    if (id ==
        0x16) { /* STN-43 analog mode: AX, AY raw absolute values, centre 80h */
        f[1] = KIND_ANALOG;
        f[4] = f[5] = 0x80; /* right stick centred */
        f[6] = (uint8_t)(cfg.axis_x_flip ? d[2] ^ 0xFFu : d[2]);
        f[7] = (uint8_t)(cfg.axis_y_flip ? d[3] ^ 0xFFu : d[3]);
    } else {
        f[1] = KIND_DIGITAL;
    }
}

void saturn_pad_decode(const uint8_t* oreg, unsigned len) {
    unsigned i = 0, port, k;
    for (port = 0; port < 2; port++) {
        uint8_t st, id, size;
        int idx = (int)port;
        if (i >= len)
            return;              /* truncated: keep previous frames */
        st = oreg[i++];          /* port status, p0077-p0078 */
        if ((st & 0x0Fu) == 0) { /* nothing connected / unknown */
            memset(frames[idx], 0xFF, 8);
            continue;
        }
        for (k = 0; k < (st & 0x0Fu);
             k++) { /* direct: 1 peripheral; multitap: skip all */
            if (i >= len)
                return;
            id = oreg[i++];
            size = id == 0xFFu ? 0 : (uint8_t)(id & 0x0Fu); /* p0080, p0083 */
            if (size == 0x0Fu || i + size > len)
                return; /* ext size (not in 15-byte mode) or truncated: keep
                           previous */
            if ((st & 0xF0u) == 0xF0u && (id == 0x02 || id == 0x16))
                build(idx, id,
                      &oreg[i]); /* other ids (incl. 15h): keep previous */
            i += size;
        }
        if ((st & 0xF0u) != 0xF0u)
            memset(frames[idx], 0xFF, 8); /* multitap unsupported */
    }
}

int saturn_pad_collect(void) {
    uint8_t buf[32];
    unsigned n;
    if (!pending)
        return 0;
    if (sf_wait()) {
        stats.miss++;
        return -1;
    }
    pending = 0;
    for (n = 0; n < 32; n++)
        buf[n] = RD8(SMPC_OREG(n));
    if (RD8(SMPC_SR) & SR_NPE)
        WR8(SMPC_IREG(0),
            IREG0_BREAK); /* remaining data: end INTBACK (p0068, p0076) */
    saturn_pad_decode(buf, 32);
    return 1;
}

/* HCNT/VCNT hold latched counter values; with EXLTEN=0 the latch happens on a
 * read of EXTEN (ST-058-R2-060194 p0037, p0042; Mednafen vdp2 read case 0x02
 * LatchHV). */
static unsigned vcnt(void) {
    (void)RD16(VDP2_EXTEN);
    return RD16(VDP2_VCNT);
}

/* INTBACK issue window on VCNT: [V-BLANK-IN +300 us, V-BLANK-OUT) in both OPE
 * modes, ST-169 p0066 (the manual allows OPE=1 from V-BLANK-IN -300 us; the
 * intersection is stricter). Ends at V-BLANK-OUT (VCNT wraps below
 * vbi_line). */
int saturn_pad_issue(void) {
    unsigned v = vcnt(), lo = cfg.vbi_line, n;
    for (n = 0; v >= lo && v < lo + LINES_300US && n < VCNT_SPIN_BOUND; n++)
        v = vcnt();
    if (v >= lo && v < lo + LINES_300US) { /* bound spent, still short */
        stats.spin_timeout++;
        return SATURN_PAD_TIMEOUT;
    }
    lo += LINES_300US;
    if (v < lo) {
        stats.window_refused++;
        return SATURN_PAD_WINDOW;
    }
    if (RD8(SMPC_SF) & 1u) {
        stats.miss++;
        return SATURN_PAD_BUSY;
    }
    WR8(SMPC_SF, 1);         /* p0016, p0024: set SF before command issue */
    WR8(SMPC_IREG(0), 0x00); /* p0068: no SMPC status */
    WR8(SMPC_IREG(1), IREG1_PEN | (cfg.ope ? IREG1_OPE : 0));
    WR8(SMPC_IREG(2), 0xF0); /* p0048, p0070 */
    WR8(SMPC_COMREG, CMD_INTBACK);
    pending = 1;
    return SATURN_PAD_ISSUED;
}

void saturn_pad_vsync(void) {
    saturn_pad_collect();
    saturn_pad_issue();
}

static int smpc_cmd(uint8_t code) {
    saturn_pad_collect(); /* do not let the command overwrite uncollected OREG
                           */
    if (sf_wait()) {
        stats.miss++;
        return -1;
    }
    WR8(SMPC_SF, 1);
    WR8(SMPC_COMREG, code);
    if (sf_wait()) { /* 30 us per p0022 */
        stats.miss++;
        return -1;
    }
    return 0;
}

int saturn_smpc_sndoff(void) { return smpc_cmd(CMD_SNDOFF); }
int saturn_smpc_sndon(void) { return smpc_cmd(CMD_SNDON); }
int saturn_smpc_resdisa(void) { return smpc_cmd(CMD_RESDISA); }
int saturn_smpc_resenab(void) { return smpc_cmd(CMD_RESENAB); }

const uint8_t* saturn_pad_frame(int idx) { return frames[idx & 1]; }
const saturn_pad_stats_t* saturn_pad_stats(void) { return &stats; }
