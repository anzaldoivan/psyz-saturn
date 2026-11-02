// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_99", _SsContNrpn2);
#else
#define NRPN_LOOP_START 20
#define NRPN_LOOP_END 30

void _SsContNrpn2(short seq_access_num, short seq_num, unsigned char data) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    switch (data) {
    case NRPN_LOOP_START:
        score->unk1B = data;
        score->unk1C = 1;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        score->next_sep_pos = score->seq_ptr;
        break;
    case NRPN_LOOP_END:
        score->unk1B = data;
        if (score->unk1D == 0) {
            score->unk15 = 0;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        } else if (score->unk1D < 0x7F) {
            score->unk1D--;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            if (score->unk1D != 0) {
                score->seq_ptr = score->next_sep_pos;
            } else {
                score->unk15 = 0;
            }
        } else {
            _SsReadDeltaValue(seq_access_num, seq_num);
            score->delta_value = 0;
            score->seq_ptr = score->next_sep_pos;
        }
        break;
    default:
        score->unk1B = data;
        score->unk1F++;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    }
}
#endif
