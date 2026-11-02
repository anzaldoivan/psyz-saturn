// SPDX-License-Identifier: MIT
#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/de_17", _SsSetNrpnVabAttr17);
#else
void _SsSetNrpnVabAttr17(short vab_id, short prog, short tone, VagAtr vag_atr,
                         short fn_idx, unsigned char attr) {
    SsUtSetReverbFeedback(attr);
}
#endif
