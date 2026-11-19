#ifndef SATURN_PAD_H
#define SATURN_PAD_H

#include <stdint.h>

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
    uint8_t ope;
    uint16_t select_chord;
    uint8_t axis_x_flip;
    uint8_t axis_y_flip;
    uint16_t vbi_line;
} saturn_pad_cfg_t;

void saturn_pad_init(const saturn_pad_cfg_t* cfg);
void saturn_pad_vsync(void);
const uint8_t* saturn_pad_frame(int idx);
uint16_t saturn_frame_vcount(void);

#endif
