// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/midimeta", _SsGetMetaEvent);
#else
void _SsGetMetaEvent(short seq_access_num, short seq_num, unsigned char meta) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 hi;
    u8 mid;
    u8 lo;

    hi = *score->seq_ptr++;
    mid = *score->seq_ptr++;
    lo = *score->seq_ptr++;
    score->unk94 = 60000000 / ((hi << 16) | (mid << 8) | lo);
    if (score->resolution * score->unk94 * 10 < VBLANK_MINUS * 60) {
        score->unk54 = score->unk52 =
            VBLANK_MINUS * 600 / (score->resolution * score->unk94);
    } else {
        score->unk52 = -1;
        score->unk54 =
            score->resolution * score->unk94 * 10 / (VBLANK_MINUS * 60);
        if (VBLANK_MINUS * 30 <
            score->resolution * score->unk94 * 10 % (VBLANK_MINUS * 60)) {
            score->unk54++;
        }
    }
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
