// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_8", _SsSetNrpnVabAttr8);
#else
void _SsSetNrpnVabAttr8(short vab_id, short prog, short tone, VagAtr vag_atr,
                        short fn_idx, unsigned char attr) {
    struct Unk adsr;

    SsUtGetVagAtr(vab_id, prog, tone, &vag_atr);
    _SsUtResolveADSR(vag_atr.adsr1, vag_atr.adsr2, &adsr);
    adsr.unkC = 0;
    adsr.unk6 = attr;
    _SsUtBuildADSR(&adsr, &vag_atr.adsr1, &vag_atr.adsr2);
    SsUtSetVagAtr(vab_id, prog, tone, &vag_atr);
}
#endif
