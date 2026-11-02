// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/vm_pb", _SsVmPBVoice);

INCLUDE_ASM("asm/nonmatchings/libsnd/vm_pb", _SsVmPitchBend);
#else
short _SsVmPBVoice(short voice, short seq_sep_no, short vabId, short prog,
                   unsigned short bend) {
    short amount = bend - 0x40;
    unsigned short tone;
    unsigned short note;
    unsigned short fine;
    int range;

    if (_svm_voice[voice].seq_sep_no != seq_sep_no ||
        _svm_voice[voice].vabId != vabId || _svm_voice[voice].prog != prog) {
        return 0;
    }
    tone = _svm_voice[voice].tone + (u8)_svm_cur.fake_program * 16;
    note = _svm_voice[voice].note;
    if (amount > 0) {
        range = amount * _svm_tn[tone].pbmax;
        note += range / 63;
        fine = (range % 63) * 2;
    } else if (amount < 0) {
        range = amount * _svm_tn[tone].pbmin;
        note = note + range / 64 - 1;
        fine = (range % 64) * 2 + 0x7F;
    } else {
        fine = 0;
    }
    _svm_cur.voice = voice;
    _svm_cur.tone = _svm_voice[voice].tone;
    _svm_sreg_buf[voice].pitch = note2pitch2(note, fine);
    _svm_sreg_dirty[voice] |= 4;
    return 1;
}

int _SsVmPitchBend(
    short seq_sep_no, short vabId, short prog, unsigned short bend) {
    short voice;
    int count = 0;

    _SsVmVSetUp(vabId, prog);
    _svm_cur.seq_sep_no = seq_sep_no;
    for (voice = 0; voice < _SsVmMaxVoice; voice++) {
        count += _SsVmPBVoice(voice, seq_sep_no, vabId, prog, bend);
    }
    return count;
}
#endif
