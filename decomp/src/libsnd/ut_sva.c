#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/ut_sva", SsUtSetVagAtr);
#else
short SsUtSetVagAtr(short vabId, short prog, short toneNum, VagAtr* vagatrptr) {
    s16 tone;
    if (_svm_vab_used[vabId] == 1) {
        _SsVmVSetUp(vabId, prog);
        tone = toneNum + _svm_cur.fake_program * 0x10;
        _svm_tn[tone].prior = vagatrptr->prior;
        _svm_tn[tone].mode = vagatrptr->mode;
        _svm_tn[tone].vol = vagatrptr->vol;
        _svm_tn[tone].pan = vagatrptr->pan;
        _svm_tn[tone].center = vagatrptr->center;
        _svm_tn[tone].shift = vagatrptr->shift;
        _svm_tn[tone].max = vagatrptr->max;
        _svm_tn[tone].min = vagatrptr->min;
        _svm_tn[tone].vibW = vagatrptr->vibW;
        _svm_tn[tone].vibT = vagatrptr->vibT;
        _svm_tn[tone].porW = vagatrptr->porW;
        _svm_tn[tone].porT = vagatrptr->porT;
        _svm_tn[tone].pbmin = vagatrptr->pbmin;
        _svm_tn[tone].pbmax = vagatrptr->pbmax;
        _svm_tn[tone].adsr1 = vagatrptr->adsr1;
        _svm_tn[tone].adsr2 = vagatrptr->adsr2;
        _svm_tn[tone].prog = vagatrptr->prog;
        _svm_tn[tone].vag = vagatrptr->vag;
        return 0;
    }
    return -1;
}
#endif
