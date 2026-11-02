// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_16", _SsSetNrpnVabAttr16);
#else
void _SsSetNrpnVabAttr16(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    SsUtSetReverbDepth(attr, attr);
}
#endif
