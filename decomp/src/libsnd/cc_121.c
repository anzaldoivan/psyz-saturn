// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_121", _SsContResetAll);
#else
void _SsContResetAll(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    SsUtReverbOff();
    _SsVmDamperOff();
    score->programs[score->channel_idx] = score->channel_idx;
    score->unk18 = 0;
    score->unk19 = 0;
    score->vol[score->channel_idx] = 0x7F;
    score->panpot[score->channel_idx] = 0x40;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
