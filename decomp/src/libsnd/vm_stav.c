// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/vm_stav", _SsVmSelectToneAndVag);
#else
unsigned char _SsVmSelectToneAndVag(u8* toneIdx, u8* vagIdx) {
    VagAtr* tn;
    u8 vagCount = 0;
    u8 i;

    for (i = 0; i < (u8)_svm_cur.prog_tones; i++) {
        tn = &_svm_tn[(u8)_svm_cur.fake_program * 16 + i];
        if (tn->min > (u8)_svm_cur.note || (u8)_svm_cur.note > tn->max) {
            continue;
        }
        vagIdx[vagCount] = tn->vag;
        toneIdx[vagCount++] = i;
    }
    return vagCount;
}
#endif
