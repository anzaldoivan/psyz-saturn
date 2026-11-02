// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_19", _SsSetNrpnVabAttr19);
#else
void _SsSetNrpnVabAttr19(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    SsUtSetReverbDelay(attr);
}
#endif
