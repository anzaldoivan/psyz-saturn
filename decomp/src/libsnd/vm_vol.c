// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/vm_vol", _SsVmSetVol);
#else
int _SsVmSetVol(short seq_sep_no, short vabId, short prog, unsigned short vol,
                unsigned short pan) {
    struct SeqStruct* score =
        &_ss_score[seq_sep_no & 0xFF][(seq_sep_no & 0xFF00) >> 8];
    short* ch_vol = &score->vol[score->channel_idx];
    short voice;
    int count = 0;
    int level;
    unsigned int vol_l;
    unsigned int vol_r;
    unsigned int tone_pan;
    VagAtr* tn;

    _SsVmVSetUp(vabId, prog);
    _svm_cur.seq_sep_no = seq_sep_no;
    if (pan == 0) {
        pan = 1;
    }
    if (vol == 0) {
        vol = 1;
    }
    for (voice = 0; voice < _SsVmMaxVoice; voice++) {
        if (_svm_voice[voice].seq_sep_no != seq_sep_no ||
            _svm_voice[voice].prog != prog ||
            _svm_voice[voice].vabId != vabId) {
            continue;
        }
        if (*ch_vol != vol && *ch_vol == 0) {
            *ch_vol = 1;
        }
        level = _svm_voice[voice].voll1 * vol / 0x7F;
        level = level * (_svm_vh->mvol * 0x3FFF) / (0x7F * 0x7F);
        tn = &_svm_tn[_svm_voice[voice].fake_program * 16 +
                      _svm_voice[voice].tone];
        vol_l = (unsigned int)(level * _svm_pg[prog].mvol * tn->vol) /
                (0x7F * 0x7F);
        vol_r = vol_l * score->volr / 0x7F;
        vol_l = vol_l * score->voll / 0x7F;
        tone_pan = tn->pan;
        if (tone_pan < 0x40) {
            vol_r = vol_r * tone_pan / 0x3F;
        } else {
            vol_l = vol_l * (0x7F - tone_pan) / 0x3F;
        }
        tone_pan = _svm_pg[_svm_voice[voice].prog].mpan;
        if (tone_pan < 0x40) {
            vol_r = vol_r * tone_pan / 0x3F;
        } else {
            vol_l = vol_l * (0x7F - tone_pan) / 0x3F;
        }
        tone_pan = pan & 0xFF;
        if (tone_pan < 0x40) {
            vol_r = vol_r * tone_pan / 0x3F;
        } else {
            vol_l = vol_l * (0x7F - tone_pan) / 0x3F;
        }
        if (_svm_stereo_mono == 1) {
            if (vol_l < vol_r) {
                vol_l = vol_r;
            } else {
                vol_r = vol_l;
            }
        }
        _svm_sreg_buf[voice].volume.left = vol_l * vol_l / 0x3FFF;
        _svm_sreg_buf[voice].volume.right = vol_r * vol_r / 0x3FFF;
        _svm_sreg_dirty[voice] |= 3;
        count++;
    }
    return count;
}
#endif
