/* saturn_pad.h -- sole SMPC owner: INTBACK pad polling -> PS1 pad frames, plus
 * SNDOFF/SNDON.
 *
 * Manual: ST-169-R1-072694 (saturn-docs-3aeb0b9a7788593e), physical pages cited
 * as pNNNN; STN-43 Ver1.00 (saturn-docs-54b9a8e669ca0e75). PS1 frame layout:
 * PsyZ psp_input.c:70-95, libetc.h:25-46, psyz/input.h:22-26. Not verified from
 * official manual: 3D pad axis direction (STN-43 silent), so axes pass through
 * unchanged and the flips default off.
 */
#ifndef SATURN_PAD_H
#define SATURN_PAD_H

#include <stdint.h>

/* Saturn pressed word (active-high) = (~1st data << 8) | ~2nd data; STN-43
 * digital format. */
#define SAT_RIGHT 0x8000u
#define SAT_LEFT 0x4000u
#define SAT_DOWN 0x2000u
#define SAT_UP 0x1000u
#define SAT_START 0x0800u
#define SAT_A 0x0400u
#define SAT_C 0x0200u
#define SAT_B 0x0100u
#define SAT_R 0x0080u
#define SAT_X 0x0040u
#define SAT_Y 0x0020u
#define SAT_Z 0x0010u
#define SAT_L 0x0008u

typedef struct {
    uint8_t
        ope; /* IREG1 bit1, p0069: 1 = not optimized (default), 0 = optimized */
    uint16_t select_chord; /* SAT_* mask emitting Select; 0 = off (default);
                              e.g. SAT_START|SAT_Z */
    uint8_t axis_x_flip;   /* default 0; direction not verified from official
                              manual */
    uint8_t axis_y_flip;   /* default 0 */
    uint16_t vbi_line; /* VCNT at V-BLANK-IN: 224 (224-line modes) default; 240
                          for 240-line */
} saturn_pad_cfg_t;

typedef struct {
    uint32_t miss;           /* SF busy at issue, or SF not done within bound */
    uint32_t window_refused; /* issue refused: outside the INTBACK window (after
                                V-BLANK-OUT) */
    uint32_t spin_timeout;   /* VCNT spin bound spent before the window */
} saturn_pad_stats_t;

#define SATURN_PAD_ISSUED 0
#define SATURN_PAD_BUSY 1   /* SF busy: no COMREG write, previous frames kept */
#define SATURN_PAD_WINDOW 2 /* outside window: no COMREG write */
#define SATURN_PAD_TIMEOUT 3 /* VCNT spin bound spent: no SMPC write */

void saturn_pad_init(const saturn_pad_cfg_t* cfg); /* NULL = defaults */
int saturn_pad_issue(
    void); /* INTBACK, call from VSync() return after V-BLANK-IN */
int saturn_pad_collect(
    void); /* 1 = frames updated, 0 = nothing pending, -1 = SF timeout */
void saturn_pad_vsync(void); /* collect, then issue */
/* Pure decode of INTBACK peripheral OREG bytes (IREG0=00h: port data from
 * OREG0, p0072-p0074); collect calls it with len 32. */
void saturn_pad_decode(const uint8_t* oreg, unsigned len);
const uint8_t* saturn_pad_frame(
    int idx); /* 8-byte PS1 frame for PsyZ pad idx 0/1 */
const saturn_pad_stats_t* saturn_pad_stats(void);

/* Sound host: same SF gate (pending INTBACK collected first). 0 ok, -1
 * busy/timeout. */
int saturn_smpc_sndoff(void);
int saturn_smpc_sndon(void);
/* Reset guard for saturn_save (p0043-p0044): same gate. 0 ok, -1 timeout. */
int saturn_smpc_resdisa(void);
int saturn_smpc_resenab(void);

#endif
