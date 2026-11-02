#include "libsnd_private.h"

#ifndef __psyz
INCLUDE_ASM("asm/nonmatchings/libsnd/ccadsr", _SsUtResolveADSR);

INCLUDE_ASM("asm/nonmatchings/libsnd/ccadsr", _SsUtBuildADSR);
#else
void _SsUtResolveADSR(u16 adsr1, u16 adsr2, struct Unk* adsr) {
    adsr->unkA = adsr1 & 0x8000;
    adsr->unkC = adsr2 & 0x8000;
    adsr->unk10 = adsr2 & 0x4000;
    adsr->unkE = adsr2 & 0x20;
    adsr->unk0 = (adsr1 >> 8) & 0x7F;
    adsr->unk2 = (adsr1 >> 4) & 0xF;
    adsr->unk4 = adsr1 & 0xF;
    adsr->unk6 = (adsr2 >> 6) & 0x7F;
    adsr->unk8 = adsr2 & 0x1F;
}

void _SsUtBuildADSR(struct Unk* adsr, u16* adsr1, u16* adsr2) {
    u16 hi1 = adsr->unkA ? 0x8000 : 0;
    u16 hi2 = adsr->unkC ? 0x8000 : 0;

    if (adsr->unk10) {
        hi2 |= 0x4000;
    }
    if (adsr->unkE) {
        hi2 |= 0x20;
    }
    *adsr1 = hi1 | ((adsr->unk0 << 8) & 0x7F00) | ((adsr->unk2 << 4) & 0xF0) |
             (adsr->unk4 & 0xF);
    *adsr2 = hi2 | ((adsr->unk6 << 6) & 0x1FC0) | (adsr->unk8 & 0x1F);
}
#endif
