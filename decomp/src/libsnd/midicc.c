// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/midicc", _SsSetControlChange);
#else
void _SsSetControlChange(
    short seq_access_num, short seq_num, unsigned char control) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 data = *score->seq_ptr++;

    switch (control) {
    case 0:
        score->vab_id = data;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    case 6:
        SsFCALL.control[CC_DATAENTRY](seq_access_num, seq_num, data);
        break;
    case 7:
        SsFCALL.control[CC_MAINVOL](seq_access_num, seq_num, data);
        break;
    case 10:
        SsFCALL.control[CC_PANPOT](seq_access_num, seq_num, data);
        break;
    case 11:
        SsFCALL.control[CC_EXPRESSION](seq_access_num, seq_num, data);
        break;
    case 64:
        SsFCALL.control[CC_DAMPER](seq_access_num, seq_num, data);
        break;
    case 91:
        SsFCALL.control[CC_EXTERNAL](seq_access_num, seq_num, data);
        break;
    case 98:
        SsFCALL.control[CC_NRPN1](seq_access_num, seq_num, data);
        break;
    case 99:
        SsFCALL.control[CC_NRPN2](seq_access_num, seq_num, data);
        break;
    case 100:
        SsFCALL.control[CC_RPN1](seq_access_num, seq_num, data);
        break;
    case 101:
        SsFCALL.control[CC_RPN2](seq_access_num, seq_num, data);
        break;
    case 121:
        SsFCALL.control[CC_RESETALL](seq_access_num, seq_num);
        break;
    default:
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        break;
    }
}
#endif
