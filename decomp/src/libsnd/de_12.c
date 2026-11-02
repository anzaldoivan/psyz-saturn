// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_12", _SsSetNrpnVabAttr12);
#else
void _SsSetNrpnVabAttr12(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    struct Unk adsr;

    SsUtGetVagAtr(vab_id, prog, tone, &vag_atr);
    _SsUtResolveADSR(vag_atr.adsr1, vag_atr.adsr2, &adsr);
    if (attr > 0 && attr < 0x40) {
        adsr.unk10 = 0;
    } else if (attr >= 0x40 && attr < 0x80) {
        adsr.unk10 = 1;
    }
    _SsUtBuildADSR(&adsr, &vag_atr.adsr1, &vag_atr.adsr2);
    SsUtSetVagAtr(vab_id, prog, tone, &vag_atr);
}
#endif
