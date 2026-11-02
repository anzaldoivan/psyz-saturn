// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/vm_key", _SsVmKeyOn);

INCLUDE_ASM("asm/nonmatchings/libsnd/vm_key", _SsVmKeyOff);

INCLUDE_ASM("asm/nonmatchings/libsnd/vm_key", _SsVmSeKeyOn);

INCLUDE_ASM("asm/nonmatchings/libsnd/vm_key", _SsVmSeKeyOff);

INCLUDE_ASM("asm/nonmatchings/libsnd/vm_key", KeyOnCheck);
#else
int _SsVmKeyOn(short seq_sep_no, short vabId, short prog, unsigned short note,
               unsigned short voll, unsigned short volr) {
    struct SeqStruct* score =
        &_ss_score[seq_sep_no & 0xFF][(seq_sep_no & 0xFF00) >> 8];
    u8 vagIdx[128];
    u8 toneIdx[128];
    u8 vagCount;
    u8 i;
    int tn;
    int ret = 0;

    if (_SsVmVSetUp(vabId, prog)) {
        return -1;
    }
    _svm_cur.seq_sep_no = seq_sep_no;
    _svm_cur.note = note;
    _svm_cur.fine = 0;
    if (seq_sep_no == 0x21) {
        _svm_cur.volume = voll;
    } else {
        _svm_cur.volume = voll * score->vol[score->channel_idx] / 0x7F;
    }
    _svm_cur.pan = volr;
    _svm_cur.mvol = _svm_pg[prog].mvol;
    _svm_cur.mpan = _svm_pg[prog].mpan;
    _svm_cur.prog_tones = _svm_pg[prog].tones;
    if ((u8)_svm_cur.fake_program >= _svm_vh->ps) {
        return -1;
    }
    if (voll == 0) {
        return _SsVmKeyOff(seq_sep_no, vabId, prog, note);
    }
    vagCount = _SsVmSelectToneAndVag(toneIdx, vagIdx);
    for (i = 0; i < vagCount; i++) {
        _svm_cur.tone_vag_idx = vagIdx[i];
        _svm_cur.tone = toneIdx[i];
        tn = _svm_cur.tone + _svm_cur.fake_program * 16;
        _svm_cur.tone_prior = _svm_tn[tn].prior;
        _svm_cur.tone_vol = _svm_tn[tn].vol;
        _svm_cur.tone_pan = _svm_tn[tn].pan;
        _svm_cur.tone_center = _svm_tn[tn].center;
        _svm_cur.tone_shift = _svm_tn[tn].shift;
        _svm_cur.tone_mode = _svm_tn[tn].mode;
        _svm_cur.tone_min = _svm_tn[tn].min;
        _svm_cur.tone_max = _svm_tn[tn].max;
        _svm_cur.voice = (u8)_SsVmAlloc(0);
        if (_svm_cur.voice < _SsVmMaxVoice) {
            _svm_voice[_svm_cur.voice].unk1b = 1;
            _svm_voice[_svm_cur.voice].unk2 = 0;
            _svm_voice[_svm_cur.voice].seq_sep_no = seq_sep_no;
            _svm_voice[_svm_cur.voice].vabId = _svm_cur.vabId;
            _svm_voice[_svm_cur.voice].fake_program = _svm_cur.fake_program;
            _svm_voice[_svm_cur.voice].prog = prog;
            if (seq_sep_no != 0x21) {
                _svm_voice[_svm_cur.voice].voll1 = voll;
            }
            _svm_voice[_svm_cur.voice].pan = volr;
            _svm_voice[_svm_cur.voice].tone = _svm_cur.tone;
            _svm_voice[_svm_cur.voice].note = note;
            _svm_voice[_svm_cur.voice].priority = _svm_cur.tone_prior;
            _svm_voice[_svm_cur.voice].vag_idx = _svm_cur.tone_vag_idx;
            _SsVmDoAllocate();
            if (_svm_cur.tone_vag_idx == 0xFF) {
                vmNoiseOn(_svm_cur.voice);
            } else {
                _SsVmKeyOnNow(vagCount, note2pitch());
            }
            ret |= 1 << _svm_cur.voice;
        } else {
            ret = -1;
        }
    }
    return ret;
}

int _SsVmKeyOff(
    short seq_sep_no, short vabId, short prog, unsigned short note) {
    u8 voice;
    int count = 0;

    for (voice = 0; voice < _SsVmMaxVoice; voice++) {
        if (_svm_voice[voice].note == note && _svm_voice[voice].prog == prog &&
            _svm_voice[voice].seq_sep_no == seq_sep_no &&
            _svm_voice[voice].vabId == vabId) {
            if (_svm_voice[voice].vag_idx == 0xFF) {
                vmNoiseOff(voice);
            } else {
                _svm_cur.voice = voice;
                _SsVmKeyOffNow(0);
            }
            count++;
        }
    }
    return count;
}

void _SsVmSeKeyOff(short vabId, short prog, unsigned short note) {
    _SsVmKeyOff(0x21, vabId, prog, note);
}

void KeyOnCheck(void) {}
#endif
