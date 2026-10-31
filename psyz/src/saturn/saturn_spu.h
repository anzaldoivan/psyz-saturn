/* saturn_spu.h -- PS1 SPU seam (psyz/spu.h: Psyz_SpuWrite, Psyz_SpuRead,
 * Psyz_SpuMemWrite, Psyz_SpuMemRead, Psyz_SpuVoiceKeyStatus) on the Saturn
 * SCSP through the ponèSound 68000 driver. Idle until the first of the five
 * calls; Psyz_VideoVSync calls saturn_spu_pump() once per frame after
 * saturn_cd_pump. The driver bytes come from the title's link
 * (saturn_spu_driver[] .. saturn_spu_driver_end[], weak); absent -> the backend
 * stays off.
 */
#ifndef SATURN_SPU_H
#define SATURN_SPU_H

#include <stdint.h>

/* Driver ready bound, pump frames. The driver's init (drv main.c:316-415)
 * spends its time in two 65535-pass ADX table loops: <= 27.3M 68000 cycles,
 * <= 146 frames at 11.29 MHz and 60 Hz without wait states; 300 is 2x that
 * and above the emulator's measured (60, 274]. */
#define SATURN_SPU_READY_FRAMES 300
#define SATURN_SPU_FPS 60 /* pump rate (NTSC; PAL 50) */

/* ready/OFF state and one counter per counted class */
typedef struct {
    unsigned state;         /* 0 idle, 1 loading, 2 ready, 3 off */
    unsigned no_driver;     /* blob absent, empty or > 0xC008 B */
    unsigned sndoff_fail;   /* SNDOFF -1: off, no SNDON */
    unsigned sndon_fail;    /* SNDON -1: off, 68000 held, no retry */
    unsigned ready_timeout; /* start != 0x7777 within READY_FRAMES */
    unsigned bad_ctrl;      /* pcmCtrl pointer outside the driver area */
    unsigned adsr;          /* voice adsr1/adsr2 (r 0x8/0xA) */
    unsigned volumex;       /* voice volumex (r 0xC) */
    unsigned loop_addr;     /* voice loop addr (r 0xE) */
    unsigned fm;            /* chan_fm 0x190/0x192 */
    unsigned noise;         /* noise_mode 0x194/0x196 */
    unsigned reverb;        /* rev_mode, rev_vol, rev_work_addr, 0x1C0-0x1FE */
    unsigned cd_vol;        /* cd_vol 0x1B0/0x1B2 */
    unsigned ex_vol;        /* ex_vol 0x1B4/0x1B6 */
    unsigned irq_addr;      /* irq_addr 0x1A4 */
    unsigned data_trans;    /* data_trans 0x1AC */
    unsigned spucnt_other;  /* spucnt bits outside enable | mute */
    unsigned fifo;          /* trans_fifo 0x1A8 */
    unsigned other;         /* any other unmapped offset 0x180-0x1FF */
    unsigned bad_offset;    /* odd or out-of-range register offsets */
    unsigned sweep;         /* volume sweep mode (bit 15) at key-on */
    unsigned stereo;        /* vol L != R at key-on (pan fixed 0) */
    unsigned pitch0;        /* key-on with pitch 0: not keyed */
    unsigned key_refused;   /* key-on outside the sample region */
    unsigned mem_refused;   /* MemWrite outside 0x1010..0x6BFE8 */
    unsigned mem_dropped;   /* MemWrite while the backend is off */
    unsigned ended;         /* one-shots ended by the pump's pass count */
    unsigned movie_resync;  /* movie writes behind the head: cursor moved */
    unsigned movie_overrun; /* movie writes ahead past the wait bound */
    unsigned movie_cur;     /* movie write cursor in the ring */
} saturn_spu_stats_t;

void saturn_spu_pump(void); /* from Psyz_VideoVSync only */
const saturn_spu_stats_t* saturn_spu_stats(void);

#ifdef SATURN_SPU_HOST
void saturn_spu_host_reset(void); /* host fixtures: back to idle */
#endif

#endif
