// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_101", _SsContRpn2);
#else
void _SsContRpn2(short seq_access_num, short seq_num, unsigned char data) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    score->unk19 = data;
    score->unk1E++;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
