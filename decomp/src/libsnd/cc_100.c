// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_100", _SsContRpn1);
#else
void _SsContRpn1(short seq_access_num, short seq_num, unsigned char data) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    score->unk18 = data;
    score->unk1E++;
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
