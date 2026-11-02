// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/tempo", _SsSndTempo);
#else
void _SsSndTempo(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    score->unkA8--;
    if (score->unkA8 < 0) {
        score->flags &= ~0x40;
        score->flags &= ~0x80;
        return;
    }
    if (score->unk4E > 0) {
        if (score->unkA8 % score->unk4E != 0) {
            return;
        }
        if (score->unk94 > score->unkAC) {
            score->unk94--;
        } else if (score->unk94 < score->unkAC) {
            score->unk94++;
        }
    } else if (score->unk94 > score->unkAC) {
        score->unk94 += score->unk4E;
        if (score->unk94 < score->unkAC) {
            score->unk94 = score->unkAC;
        }
    } else if (score->unk94 < score->unkAC) {
        score->unk94 -= score->unk4E;
        if (score->unk94 > score->unkAC) {
            score->unk94 = score->unkAC;
        }
    }
    score->unk54 = score->resolution * score->unk94 * 10 / (VBLANK_MINUS * 60);
    if (score->unk54 <= 0) {
        score->unk54 = 1;
    }
    if (score->unkA8 == 0 || score->unk94 == score->unkAC) {
        score->flags &= ~0x40;
        score->flags &= ~0x80;
    }
}
#endif
