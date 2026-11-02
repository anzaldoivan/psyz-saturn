// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_1", _SsSetNrpnVabAttr1);
#else
void _SsSetNrpnVabAttr1(short vab_id, short prog, short tone, VagAtr vag_atr,
                        short fn_idx, unsigned char attr) {
    SsUtGetVagAtr(vab_id, prog, tone, &vag_atr);
    vag_atr.mode = attr;
    SsUtSetVagAtr(vab_id, prog, tone, &vag_atr);
    if (attr == 0) {
        SsUtReverbOff();
    } else if (attr == 4) {
        SsUtReverbOn();
    }
}
#endif
