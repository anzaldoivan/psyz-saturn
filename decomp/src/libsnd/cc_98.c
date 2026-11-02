// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_98", _SsContNrpn1);
#else
void _SsContNrpn1(short seq_access_num, short seq_num, unsigned char data) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    SndSsMarkCallbackProc cb;

    if (score->unk1C == 1 && score->unk15 == 0) {
        score->unk1D = data;
        score->unk1C = 0;
        score->unk15 = 1;
    } else if (score->unk1B != 30 && score->unk1B != 20) {
        score->fn_idx = data;
        score->unk1C = 0;
        score->unk1F++;
    }
    if (score->unk1B == 40) {
        cb = _SsMarkCallback[seq_access_num][seq_num];
        if (cb != NULL) {
            cb(seq_access_num, seq_num, data);
        }
    }
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
