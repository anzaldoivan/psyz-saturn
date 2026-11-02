#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/cc_6", _SsContDataEntry);
#else
typedef void (*SetVabAttr)(short, short, short, VagAtr, short, unsigned char);

void _SsContDataEntry(short seq_access_num, short seq_num, unsigned char data) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 channel = score->channel_idx;
    ProgAtr prog_atr = {0};
    VagAtr vag_atr = {0};
    SetVabAttr set_attr;
    short i;

    SsUtGetProgAtr(score->vab_id, score->programs[channel], &prog_atr);
    if (score->unk1C == 1 && score->unk15 == 0) {
        score->unk1D = data;
        score->unk1C = 0;
        score->unk15 = 1;
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
    } else if (score->unk1E == 2) {
        if (score->unk19 == 0) {
            for (i = 0; i < prog_atr.tones; i++) {
                SsUtGetVagAtr(
                    score->vab_id, score->programs[channel], i, &vag_atr);
                if (score->unk18 == 0) {
                    vag_atr.pbmin = data & 0x7F;
                    vag_atr.pbmax = data & 0x7F;
                }
                SsUtSetVagAtr(
                    score->vab_id, score->programs[channel], i, &vag_atr);
            }
        }
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        score->unk1E = 0;
    } else if (score->unk1F == 2) {
        set_attr = (SetVabAttr)SsFCALL.ccentry[score->fn_idx];
        if (score->unk1B == 16) {
            for (i = 0; i < prog_atr.tones; i++) {
                set_attr(score->vab_id, score->programs[channel], i, vag_atr,
                         score->fn_idx, data);
            }
        } else {
            set_attr(score->vab_id, score->programs[channel], score->unk1B,
                     vag_atr, score->fn_idx, data);
        }
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
        score->unk1F = 0;
    } else {
        score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
    }
}
#endif
