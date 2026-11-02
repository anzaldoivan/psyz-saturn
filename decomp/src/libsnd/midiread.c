// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/midiread", _SsSeqPlay);

INCLUDE_ASM("asm/nonmatchings/libsnd/midiread", _SsSeqGetEof);

INCLUDE_ASM("asm/nonmatchings/libsnd/midiread", _SsGetSeqData);
#else
void _SsSeqPlay(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    int ticks;

    if (score->delta_value - score->unk54 > 0) {
        if (score->unk52 > 0) {
            score->unk52--;
        } else if (score->unk52 == 0) {
            score->unk52 = score->unk54;
            score->delta_value--;
        } else {
            score->delta_value -= score->unk54;
        }
    } else if (score->delta_value <= score->unk54) {
        ticks = score->delta_value;
        do {
            do {
                _SsGetSeqData(seq_access_num, seq_num);
            } while (score->delta_value == 0);
            ticks += score->delta_value;
        } while (ticks < score->unk54);
        score->delta_value = ticks - score->unk54;
    }
}

void _SsSeqGetEof(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];

    score->unk21++;
    if (score->l_count == 0 || score->unk21 < score->l_count) {
        score->unk88 = 0;
        score->unk1C = 0;
        score->delta_value = 0;
        if (score->flags & SEQ_FLAG_400) {
            score->seq_ptr = score->loop_pos;
        } else {
            score->seq_ptr = score->read_pos;
        }
        if (score->l_count != 0) {
            score->next_sep_pos = score->seq_ptr;
        }
        return;
    }
    score->flags &= ~SEQ_FLAG_1;
    score->flags &= ~SEQ_FLAG_8;
    score->flags &= ~SEQ_FLAG_2;
    score->flags |= SEQ_FLAG_200;
    score->flags |= SEQ_FLAG_4;
    score->play_mode = 0;
    if (score->flags & SEQ_FLAG_400) {
        score->next_sep_pos = score->loop_pos;
    } else {
        score->next_sep_pos = score->read_pos;
    }
    if (score->unk22 != 0xFF) {
        score->play_mode = 0;
        _SsSndNextSep(score->unk22, score->unk23);
        _SsVmSeqKeyOff(seq_access_num | (seq_num << 8));
    }
    _SsVmSeqKeyOff(seq_access_num | (seq_num << 8));
    score->delta_value = score->unk54;
}

void _SsGetSeqData(short seq_access_num, short seq_num) {
    struct SeqStruct* score = &_ss_score[seq_access_num][seq_num];
    u8 data;
    u8 note;
    u8 vel;

    data = *score->seq_ptr++;
    if ((score->flags & (SEQ_FLAG_400 | SEQ_FLAG_1)) ==
            (SEQ_FLAG_400 | SEQ_FLAG_1) &&
        score->seq_ptr == (u8*)(uintptr_t)score->unk10 + 1) {
        _SsSeqGetEof(seq_access_num, seq_num);
        return;
    }
    if (data & 0x80) {
        score->channel_idx = data & 0xF;
        switch (data & 0xF0) {
        case 0x90:
            score->running_status = 0x90;
            note = *score->seq_ptr++;
            vel = *score->seq_ptr++;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            SsFCALL.noteon(seq_access_num, seq_num, note, vel);
            break;
        case 0xB0:
            score->running_status = 0xB0;
            SsFCALL.control[CC_NUMBER](
                seq_access_num, seq_num, *score->seq_ptr++);
            break;
        case 0xC0:
            score->running_status = 0xC0;
            SsFCALL.programchange(seq_access_num, seq_num, *score->seq_ptr++);
            break;
        case 0xE0:
            score->running_status = 0xE0;
            score->seq_ptr++;
            SsFCALL.pitchbend(seq_access_num, seq_num);
            break;
        case 0xF0:
            score->running_status = 0xFF;
            score->channel_idx = data & 0xF;
            data = *score->seq_ptr++;
            if (data == 0x2F) {
                _SsSeqGetEof(seq_access_num, seq_num);
            } else {
                SsFCALL.metaevent(seq_access_num, seq_num, data);
            }
            break;
        }
    } else {
        switch (score->running_status) {
        case 0x90:
            vel = *score->seq_ptr++;
            score->delta_value = _SsReadDeltaValue(seq_access_num, seq_num);
            SsFCALL.noteon(seq_access_num, seq_num, data, vel);
            break;
        case 0xB0:
            SsFCALL.control[CC_NUMBER](seq_access_num, seq_num, data);
            break;
        case 0xC0:
            SsFCALL.programchange(seq_access_num, seq_num, data);
            break;
        case 0xE0:
            SsFCALL.pitchbend(seq_access_num, seq_num);
            break;
        case 0xFF:
            if (data == 0x2F) {
                _SsSeqGetEof(seq_access_num, seq_num);
            } else {
                SsFCALL.metaevent(seq_access_num, seq_num, data);
            }
            break;
        }
    }
}
#endif
