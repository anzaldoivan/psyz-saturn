#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_11", _SsContExpression);
#else
void _SsContExpression(
    short seq_access_num, short seq_num, unsigned char expression) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 channel = score->channel_idx;

    _SsVmSetProgVol(score->vab_id, score->programs[channel], expression);
    _SsVmSetVol(
        seq_access_num | (seq_num << 8), score->vab_id,
        score->programs[channel], score->vol[channel], score->panpot[channel]);
    score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
}
#endif
