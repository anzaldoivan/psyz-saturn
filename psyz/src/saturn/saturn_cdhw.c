/* saturn_cdhw.c -- CD block transport behind the saturn_cd.h hardware port.
 * Polled and bounded: no interrupt, no callback; every wait is a fixed bound.
 * Citations: ST-162-062094 physical pages pNNNN of saturn-docs-471cb1b4ff67952d
 * (System Library User's Guide). Command encodings follow libyaul 6012f79f,
 * MIT, Copyright (c) 2012-2023 Israel Jacquez:
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 * Streaming: one PlayDisc covers a range and the drive pauses at its end
 * (p0025); sectors queue in the 200-sector buffer (p0011), the drive pauses
 * while it is full and resumes as it drains (p0026). Reads at the open play's
 * cursor issue no command, so the drive does not re-seek per read as on a new
 * Play (Mednafen 1.32.1 src/ss/cdb.cpp:1946-1957,2200-2204). A poll takes up
 * to CDHW_BURST sectors in one GetThenDeleteSectorData (the sector count
 * deletes the range, p0084): one DRDY and one EndDataTransfer per command
 * (p0020).
 * Buffer full: BFUL is counted and cleared once per poll (200 sectors, p0011;
 * pause on full and resume on space, p0026).
 * Transfer: one CPU-DMAC channel 1 burst per take, source DATATRNS fixed
 * (16-bit access, ST-162-062094 p0015), destination incrementing, length in
 * bytes, then cpu_cache_purge (the DMAC writes behind the CPU cache).
 * CPU-DMA is a CD transfer mode of SBL Cinepak (MANCPK.DOC:156-169,
 * SEGA_CPK.H:145-159 CPK_TRMODE_SDMA); the SBL Stream System links the DMA
 * and Cache libraries (SBL601 MANSTM.TXT:125-126). Sequence as libyaul
 * 6012f79f cd-block_init.c:213-270 cd_block_transfer_data_dmac; channel 0 is
 * libyaul's (vdp_init.c:55 cpu_dmac_memset).
 */
#include "saturn_cd.h"
#include <stdint.h>

/* libyaul 6012f79f cd-block-internal.h:41-47; not verified from official manual
 */
#define CD_BASE 0x25890000u
#define DTR (CD_BASE + 0x00u)
#define HIRQ (CD_BASE + 0x08u)
#define CR1 (CD_BASE + 0x18u)
#define CR2 (CD_BASE + 0x1Cu)
#define CR3 (CD_BASE + 0x20u)
#define CR4 (CD_BASE + 0x24u)

#define CMOK 0x0001u /* ST-162-062094 p0016; libyaul cd-block-internal.h */
#define DRDY 0x0002u /* ST-162-062094 p0016; libyaul cd-block-internal.h */
#define ESEL 0x0040u /* ST-162-062094 p0016; libyaul cd-block-internal.h */
#define BFUL 0x0008u /* ST-162-062094 p0016; libyaul cd-block-internal.h */

#define ST_PAUSE 0x01u   /* ST-162-062094 p0047 */
#define ST_OPEN 0x06u    /* ST-162-062094 p0047 */
#define ST_NO_DISC 0x07u /* ST-162-062094 p0047 */
#define ST_ERROR 0x09u   /* ST-162-062094 p0047 */
#define ST_FATAL 0x0Au   /* ST-162-062094 p0047 */
#define ST_WAIT 0x80u    /* ST-162-062094 p0047 */
#define ST_REJECT 0xFFu  /* ST-162-062094 p0047 */

#define CDHW_CMOK_BOUND                                                        \
    20000u /* HIRQ reads; design value, not a manual value */
#define CDHW_FLAG_BOUND                                                        \
    20000u             /* HIRQ reads; design value, not a manual value */
#define TOC_WORDS 204u /* 102 entries x 2 words; layout: not verified */
#define CDHW_BURST 16u /* sectors per GetThenDelete; design value */

#ifdef SATURN_CD_HOST
uint16_t saturn_cdhw_rd16(uint32_t a);
void saturn_cdhw_wr16(uint32_t a, uint16_t v);
void saturn_cdhw_dma16(void* dst, uint32_t words);
#define RD16(a) saturn_cdhw_rd16(a)
#define WR16(a, v) saturn_cdhw_wr16((a), (uint16_t)(v))
#else
#include <yaul.h>
#define RD16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define WR16(a, v) (*(volatile uint16_t*)(uintptr_t)(a) = (uint16_t)(v))
#endif

static struct {
    int ready;        /* setup() done */
    int kind;         /* 0 idle, 1 read, 2 seek */
    int err;          /* take() failed; next poll reports SATCD_HW_ERROR */
    int open;         /* a play is open at next with play_left sectors */
    uint32_t pending; /* sectors waiting in DTR for take() */
    uint32_t left;
    uint32_t next, play_left;
} hw;

#define CDHW_DMAC_CH 1u /* channel 0 is libyaul's (vdp_init.c:55) */

/* DATATRNS -> dst, `words` halfwords; 0 done, -1 the channel still busy at
 * CDHW_FLAG_BOUND status reads (bound: design value, not a manual value) */
static int cdhw_dma16(void* dst, uint32_t words) {
#ifdef SATURN_CD_HOST
    saturn_cdhw_dma16(dst, words);
    return 0;
#else
    cpu_dmac_status_t s;
    uint32_t i;
    const cpu_dmac_cfg_t cfg = {
        .channel = CDHW_DMAC_CH,
        .src_mode = CPU_DMAC_SOURCE_FIXED,
        .src = DTR,
        .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
        .dst = (uint32_t)(uintptr_t)dst,
        .len = words * 2u,
        .stride = CPU_DMAC_STRIDE_2_BYTES,
        .bus_mode = CPU_DMAC_BUS_MODE_BURST,
        .ihr = NULL,
        .ihr_work = NULL};
    cpu_dmac_channel_config_set(&cfg);
    cpu_dmac_channel_start(CDHW_DMAC_CH);
    for (i = 0; i < CDHW_FLAG_BOUND; i++) {
        cpu_dmac_status_get(&s);
        if ((s.channel_busy & (1u << CDHW_DMAC_CH)) == 0)
            break;
    }
    if (i == CDHW_FLAG_BOUND)
        cpu_dmac_channel_stop(CDHW_DMAC_CH);
    cpu_cache_purge();
    return i == CDHW_FLAG_BOUND ? -1 : 0;
#endif
}

static int wait_bit(uint16_t f, uint32_t bound) {
    uint32_t i;
    for (i = 0; i < bound; i++)
        if (RD16(HIRQ) & f)
            return 0;
    return -1;
}

/* Status = CR1 high byte; libyaul 6012f79f cd-block_execute.c:24-80; not
 * verified from official manual */
static int cmd(
    uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4, uint16_t out[4]) {
    uint8_t st;
    if (wait_bit(CMOK, CDHW_CMOK_BOUND) != 0)
        return -1;
    WR16(HIRQ, RD16(HIRQ) & ~CMOK); /* write 0 clears: ST-162-062094 p0016 */
    WR16(CR1, c1);
    WR16(CR2, c2);
    WR16(CR3, c3);
    WR16(CR4, c4);
    if (wait_bit(CMOK, CDHW_CMOK_BOUND) != 0)
        return -1;
    out[0] = RD16(CR1);
    out[1] = RD16(CR2);
    out[2] = RD16(CR3);
    out[3] = RD16(CR4);
    st = (uint8_t)(out[0] >> 8);
    return (st == ST_REJECT || (st & ST_WAIT)) ? -1 : 0;
}

static int bad_status(uint8_t st) {
    st &= 0x0Fu;
    return st == ST_OPEN || st == ST_NO_DISC || st == ST_ERROR ||
           st == ST_FATAL;
}

/* Waits for f, then clears it (write 0 clears: ST-162-062094 p0016). */
static int flag_wait(uint16_t f) {
    if (wait_bit(f, CDHW_FLAG_BOUND) != 0)
        return -1;
    WR16(HIRQ, (uint16_t)~f);
    return 0;
}

/* ResetSelector partition 0; CDC_ResetSelector p0062; libyaul 6012f79f
 * cd-block_cmds.c:504-514; not verified from official manual */
static int reset_part0(void) {
    uint16_t o[4];
    if (cmd(0x4800u, 0, 0x0000u, 0, o) != 0)
        return -1;
    return flag_wait(ESEL);
}

/* EndDataTransfer; CDC_DataEnd p0069; libyaul 6012f79f cd-block_cmds.c:203-213;
 * not verified from official manual */
static int end_xfer(void) {
    uint16_t o[4];
    int r = cmd(0x0600u, 0, 0, 0, o);
    WR16(HIRQ, (uint16_t)~DRDY);
    return r;
}

static int setup(void) {
    uint16_t o[4];
    if (hw.ready)
        return 0;
    /* SetSectorLength 2048; CDC_SetSctLen p0083 (CDC_SLEN_2048, the initial
     * value); libyaul 6012f79f cd-block_cmds.c:592-600 */
    if (cmd(0x6000u, 0x0000u, 0, 0, o) != 0)
        return -1;
    if (reset_part0() != 0)
        return -1;
    /* SetCDDeviceConnection filter 0; CDC_CdSetCon p0074; libyaul 6012f79f
     * cd-block_cmds.c:330-340; not verified from official manual */
    if (cmd(0x3000u, 0, 0x0000u, 0, o) != 0)
        return -1;
    hw.ready = 1;
    return 0;
}

static uint32_t entry(const uint16_t* t, unsigned i) {
    return (uint32_t)t[2 * i] << 16 | t[2 * i + 1];
}

int saturn_cdhw_toc(uint32_t* fad, int max, uint32_t* leadout) {
    static uint16_t t[TOC_WORDS];
    uint16_t o[4];
    uint32_t lo;
    unsigned i;
    int count = 0;
    if (setup() != 0)
        return 0;
    /* GetToc; CDC_TgetToc p0062,p0065 (102x4 layout: not verified); libyaul
     * 6012f79f cd-block_cmds.c:103-113 */
    if (cmd(0x0200u, 0, 0, 0, o) != 0)
        return 0;
    if (bad_status((uint8_t)(o[0] >> 8)))
        return 0;
    if (flag_wait(DRDY) != 0)
        return 0;
    for (i = 0; i < TOC_WORDS; i++)
        t[i] = RD16(DTR);
    if (end_xfer() != 0)
        return 0;
    lo = entry(t, 101); /* A2 lead-out */
    if (lo == 0xFFFFFFFFu)
        return 0;
    while (count < 99 && entry(t, (unsigned)count) != 0xFFFFFFFFu) {
        if (count < max)
            fad[count] = entry(t, (unsigned)count) & 0xFFFFFFu;
        count++;
    }
    *leadout = lo & 0xFFFFFFu;
    return count;
}

static int play(uint32_t fad, uint32_t n) {
    uint16_t o[4];
    uint32_t pn = n;
    hw.open = 0;
    if (setup() != 0 || reset_part0() != 0)
        return -1;
    /* PlayDisc FAD + count; CDC_CdPlay p0070,p0057 (0x80 marker not verified);
     * libyaul 6012f79f cd-block_cmds.c:226-241 */
    if (cmd((uint16_t)(0x1080u | (fad >> 16 & 0xFFu)), (uint16_t)fad,
            (uint16_t)(0x0080u | (pn >> 16 & 0xFFu)), (uint16_t)pn, o) != 0 ||
        bad_status((uint8_t)(o[0] >> 8)))
        return -1;
    hw.pending = 0;
    hw.err = 0;
    return 0;
}

int saturn_cdhw_play(uint32_t fad, uint32_t n) {
    if (play(fad, n) != 0)
        return -1;
    hw.kind = 0;
    hw.left = 0;
    hw.open = 1;
    hw.next = fad;
    hw.play_left = n;
    return 0;
}

int saturn_cdhw_read(uint32_t fad, uint32_t n) {
    if (hw.open && fad == hw.next && n <= hw.play_left) {
        hw.next += n;
        hw.play_left -= n;
    } else if (play(fad, n) != 0) {
        return -1;
    }
    hw.kind = 1;
    hw.left = n;
    hw.pending = 0;
    hw.err = 0;
    return 0;
}

int saturn_cdhw_seek(uint32_t fad) {
    uint16_t o[4];
    hw.open = 0;
    if (setup() != 0)
        return -1;
    /* SeekDisc; CDC_CdSeek p0071; libyaul 6012f79f cd-block_cmds.c:253-263 */
    if (cmd((uint16_t)(0x1180u | (fad >> 16 & 0xFFu)), (uint16_t)fad, 0, 0,
            o) != 0 ||
        bad_status((uint8_t)(o[0] >> 8)))
        return -1;
    hw.kind = 2;
    hw.err = 0;
    return 0;
}

static int fail(void) {
    hw.kind = 0;
    return SATCD_HW_ERROR;
}

int saturn_cdhw_poll(void) {
    uint16_t o[4];
    uint32_t n;
    if (hw.kind == 0)
        return SATCD_HW_BUSY;
    if (hw.err) {
        hw.err = 0;
        return fail();
    }
    if (hw.kind == 2) {
        /* GetStatus; CDC_GetCurStat p0062; libyaul 6012f79f
         * cd-block_cmds.c:45-55 */
        if (cmd(0x0000u, 0, 0, 0, o) != 0 || bad_status((uint8_t)(o[0] >> 8)))
            return fail();
        if ((o[0] >> 8 & 0x0Fu) == ST_PAUSE) {
            hw.kind = 0;
            return SATCD_HW_DONE;
        }
        return SATCD_HW_BUSY;
    }
    /* GetSectorNumber partition 0 (no manual hit: not verified); libyaul
     * 6012f79f cd-block_cmds.c:563-571 */
    if (cmd(0x5100u, 0, 0x0000u, 0, o) != 0 || bad_status((uint8_t)(o[0] >> 8)))
        return fail();
    if (RD16(HIRQ) & BFUL) {
        saturn_cd_stats.bful++;
        WR16(HIRQ, (uint16_t)~BFUL); /* write 0 clears: ST-162-062094 p0016 */
    }
    if (o[3] == 0)
        return SATCD_HW_BUSY;
    n = o[3] < hw.left ? o[3] : hw.left;
    if (n > CDHW_BURST)
        n = CDHW_BURST;
    /* GetThenDeleteSectorData n sectors from offset 0; CDC_GetdelSctData
     * p0084; libyaul 6012f79f cd-block_cmds.c:656-667 */
    if (cmd(0x6300u, 0, 0x0000u, (uint16_t)n, o) != 0 ||
        bad_status((uint8_t)(o[0] >> 8)) || flag_wait(DRDY) != 0)
        return fail();
    hw.pending = n;
    return SATCD_HW_SECTOR;
}

uint32_t saturn_cdhw_take(void* dst) {
    uint32_t n = hw.pending;
    int e;
    if (n == 0)
        return 0;
    /* big-endian halfwords as the bus delivers them */
    e = cdhw_dma16(dst, n * (SATCD_SECTOR / 2));
    if (end_xfer() != 0 || e != 0)
        hw.err = 1;
    hw.pending = 0;
    hw.left -= n;
    if (hw.left == 0)
        hw.kind = 0;
    return n;
}

void saturn_cdhw_abort(void) {
    uint16_t o[4];
    if (hw.pending)
        (void)end_xfer();
    /* SeekDisc 0xFFFFFF = pause (rule not verified); CDC_CdSeek p0071; libyaul
     * 6012f79f cd-block_cmds.c:253-263 */
    (void)cmd(0x11FFu, 0xFFFFu, 0, 0, o);
    (void)reset_part0();
    hw.kind = 0;
    hw.pending = 0;
    hw.err = 0;
    hw.left = 0;
    hw.open = 0;
}
