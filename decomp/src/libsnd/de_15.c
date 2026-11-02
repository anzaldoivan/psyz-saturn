// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_15", _SsSetNrpnVabAttr15);
#else
void _SsSetNrpnVabAttr15(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    SsUtSetReverbType(attr);
}
#endif
