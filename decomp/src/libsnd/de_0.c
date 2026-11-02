// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_0", _SsSetNrpnVabAttr0);
#else
void _SsSetNrpnVabAttr0(short vab_id, short prog, short tone, VagAtr vag_atr,
                        short fn_idx, unsigned char attr) {
    SsUtGetVagAtr(vab_id, prog, tone, &vag_atr);
    vag_atr.prior = attr;
    SsUtSetVagAtr(vab_id, prog, tone, &vag_atr);
}
#endif
