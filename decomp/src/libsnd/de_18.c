// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_18", _SsSetNrpnVabAttr18);
#else
void _SsSetNrpnVabAttr18(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    SsUtSetReverbDelay(attr);
}
#endif
