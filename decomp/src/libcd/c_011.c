#include "common.h"
#include <libcd.h>

extern volatile u16* HeaderArea__dot__2;

extern volatile s32* D_800B54A0;
extern volatile s32* D_800B54A4;
extern volatile s32* D_800B54B0;
extern volatile s32* D_800B54C0;
extern s32 debug_cause;
extern s32 Stframe_no;
extern s16 Stsector_offset;
extern s32 StRgb24;
extern s32 StMode;
extern void (*StFunc2)(void);
extern s32 StCdIntrFlag;
extern s32 StCHANNEL;
extern s32 StStartFrame;
extern s32 CChannel;
extern s32 StFinalSector;
extern s32 StEmu_Idx;
extern s32 StRingIdx1;
extern s32 StRingIdx2;
extern s32 StEmu_Addr;
extern u32 StEndFrame;
extern s32 StSTART_FLAG;
extern u16* StRingBase;
extern StHEADER* StRingAddr;
extern s32 StRingSize;

extern volatile u8 *D_800B5490, *D_800B5498, *D_800B549C;
extern volatile s32* D_800B54A8;
extern volatile u8* D_800B54AC;
void mem2mem(s32* dst, s32* src, u32 num, u32 completion);
void dma_execute(s32 channel, void* address, u32 blocks, u32 words, u32 control,
                 u_char completion, u32 unused);
int init_ring_status(s32 start, u32 count);
void data_ready_callback(void);

#ifndef __psyz
#define CD_REG_WRITE(reg, value) (*(reg) = (value))
#define CD_DMA_BUSY() (*D_800B54C0 & 0x01000000)
#define MDEC_OUT_BUSY() (*D_800B54B0 & 0x01000000)
#else
#include <psyz.h>
volatile u16* HeaderArea__dot__2;
s32 debug_cause;
#include "libcd_private.h"
#define CD_REG_WRITE(reg, value)
#define CD_DMA_BUSY() 0
#define MDEC_OUT_BUSY() (Psyz_DmaRead(DMA_CHANNEL_MDEC_OUT, 2) & 0x01000000)
#endif

void StCdInterrupt(void) {
    volatile s16 subroutine_arg8[4];
    CdlLOC loc;
    u_char result[8];
    u32* var_a1;
    s32 var_t0;
    u32* var_a0;
    u32 var_v1_2;
    u32 var_v1_3;
    u8* var_v1;

    if (StFinalSector == 1) {
        return;
    }
    if ((StRgb24 != 0) && MDEC_OUT_BUSY()) {
        StCdIntrFlag = 1;
        if (StEmu_Addr != 0) {
            StEmu_Idx++;
        }
        debug_cause = 1;
        return;
    }
    if (CdReady(1, result) == CdlDiskError) {
        return;
    }
    subroutine_arg8[1] = result[0];
    subroutine_arg8[2] = result[1];
    if (subroutine_arg8[1] & 4) {
        debug_cause = 3;
        return;
    }
    HeaderArea__dot__2 = (u16*)&StRingAddr[StRingIdx1];
    if (HeaderArea__dot__2[0] != 0) {
        if (StEmu_Addr != 0) {
            StEmu_Idx++;
        }
        debug_cause = 4;
        return;
    }
    CD_REG_WRITE(D_800B5490, 0);
    CD_REG_WRITE(D_800B549C, 0);
    CD_REG_WRITE(D_800B5490, 0);
    CD_REG_WRITE(D_800B549C, 0x80);
    CD_REG_WRITE(D_800B54A0, 0x20943);
    CD_REG_WRITE(D_800B54A4, 0x1323);
    if (StMode == 0) {
#ifdef __psyz
        u32 subheader[2];
        CD_getsector(&loc, 1);
        CD_getsector(subheader, 2);
#else
        var_v1 = (u8*)&loc;
        do {
            *var_v1++ = *D_800B5498;
        } while (var_v1 < (u8*)(&loc + 1));
        for (var_v1_2 = 0; var_v1_2 < 8; var_v1_2++) {
            *D_800B5498;
        }
#endif
    }
    var_t0 = 0x11000000;
    if (StEmu_Addr != 0) {
        mem2mem((s32*)HeaderArea__dot__2,
                (s32*)(StEmu_Addr + (StEmu_Idx << 0xB)), 8, 0);
    } else {
        dma_execute(3, HeaderArea__dot__2, 0, 8, var_t0, 0, 0);
    }
    while (CD_DMA_BUSY()) {
    }
    ((StHEADER*)HeaderArea__dot__2)->loc = loc;
    CD_REG_WRITE(D_800B54A0, 0x20843);
    CD_REG_WRITE(D_800B54A4, 0x1325);
    if ((StSTART_FLAG == 1) && (StStartFrame != 0)) {
        if (StStartFrame != HeaderArea__dot__2[4]) {
            HeaderArea__dot__2[0] = 0;
            if (StEmu_Addr != 0) {
                StEmu_Idx++;
            }
            return;
        }
        StSTART_FLAG = 0;
    }
    if ((HeaderArea__dot__2[0] != 0x160) ||
        (((HeaderArea__dot__2[1] >> 0xA) & 0x1F) != CChannel)) {
        if (StEmu_Addr != 0) {
            StEmu_Idx = 0;
        } else {
            HeaderArea__dot__2[0];
        }
        debug_cause = 5;
        HeaderArea__dot__2[0] = 0;
        return;
    }
    if ((Stsector_offset != HeaderArea__dot__2[2]) ||
        ((Stframe_no != 0) && (Stframe_no != HeaderArea__dot__2[4]))) {
        Stframe_no = 0;
        Stsector_offset = 0;
        init_ring_status(StRingIdx2, StRingIdx1 - StRingIdx2);
        StRingIdx1 = StRingIdx2;
        HeaderArea__dot__2[0] = 0;
        if (StEmu_Addr != 0) {
            StEmu_Idx++;
        }
        debug_cause = 6;
        return;
    }
    if (HeaderArea__dot__2[2] == 0) {
        Stsector_offset = 0;
        Stframe_no = HeaderArea__dot__2[4];
        if ((StEndFrame != 0) && (Stframe_no >= StEndFrame)) {
            Stframe_no = 0;
            Stsector_offset = 0;
            init_ring_status(StRingIdx2, StRingIdx1 - StRingIdx2);
            StRingIdx1 = StRingIdx2;
            HeaderArea__dot__2[0] = 0;
            StSTART_FLAG = 1;
            if (StFunc2 != NULL) {
                StFunc2();
            }
            if (StEmu_Addr != 0) {
                StEmu_Idx++;
            }
            debug_cause = 7;
            return;
        }
        if ((u32)(StRingSize - StRingIdx1 - 1) < HeaderArea__dot__2[3]) {
            if (StEndFrame == 0) {
                HeaderArea__dot__2[0] = 1;
                StSTART_FLAG = 1;
                if (StFunc2 != NULL) {
                    StFunc2();
                }
                if (StEmu_Addr != 0) {
                    StEmu_Idx++;
                }
                debug_cause = 8;
                return;
            }
            if ((s16)StRingAddr->id != 0) {
                HeaderArea__dot__2[0] = 0;
                if (StEmu_Addr != 0) {
                    StEmu_Idx++;
                }
                debug_cause = 9;
                return;
            }
            HeaderArea__dot__2[0] = 1;
            var_a1 = (u32*)StRingAddr;
            var_a0 = (u32*)HeaderArea__dot__2;
            StRingIdx1 = 0;
            for (var_v1_3 = 0; var_v1_3 < 8; var_v1_3++) {
                *var_a1++ = *var_a0++;
            }
            HeaderArea__dot__2 = (u16*)StRingAddr;
        }
        StRingIdx2 = StRingIdx1;
    }
    debug_cause = 10;
    Stsector_offset++;
    StRingBase = (u16*)(&StRingAddr[StRingSize] + (StRingIdx1 * 0x3F));

    if (StRgb24 != 0) {
        var_t0 = 0x11000000;
        CD_REG_WRITE(D_800B54A0, 0x20943);
        CD_REG_WRITE(D_800B54A4, 0x1323);
    } else {
        CD_REG_WRITE(D_800B54A0, 0x21020843);
        var_t0 = 0x11400100;
    }
    if ((HeaderArea__dot__2[3] - 1) == HeaderArea__dot__2[2]) {
        StFinalSector = 1;
        if (StEmu_Addr != 0) {
            mem2mem((s32*)StRingBase,
                    (s32*)(StEmu_Addr + (StEmu_Idx << 0xB) + 0x20), 0x1F8, 1);
            StEmu_Idx++;
        } else {
            dma_execute(3, StRingBase, 0, 0x1F8, var_t0, 1, 0);
        }
        Stsector_offset = 0;
        Stframe_no = 0;
        CChannel = StCHANNEL;
    } else {
        if (StEmu_Addr != 0) {
            mem2mem((s32*)StRingBase,
                    (s32*)(StEmu_Addr + (StEmu_Idx << 0xB) + 0x20), 0x1F8, 0);
            StEmu_Idx++;
        } else {
            dma_execute(3, StRingBase, 0, 0x1F8, var_t0, 0, 0);
        }
    }
    CD_REG_WRITE(D_800B54A4, 0x1325);
    HeaderArea__dot__2[0] = 3;
    StRingIdx1 += 1;
    if ((StEmu_Addr != 0) && (StFinalSector != 0)) {
        data_ready_callback();
    }
}

void mem2mem(s32* dst, s32* src, u32 num, u32 arg3) {
    u32 i;
    for (i = 0; i < num; i++) {
        *dst++ = *src++;
    }
}

#ifndef __psyz
typedef struct {
    s32 madr;
    s32 bcr;
    s32 chcr;
    s32 unk;
} DMA_CONTROL;

void dma_execute(s32 id, void* addr, u32 blocks, u32 size, u32 control,
                 u_char arg5, u32 arg6) {
    s32 i;
    s32 temp;
    s32 temp2;
    u8 v;
    volatile s32 ret;
    volatile s32* ptr;

    for (i = 0; *(volatile u32*)(0x1F801088 + (id << 4)) & 0x01000000; i++) {
        if (i == 0x10000) {
            printf("DMA STATUS ERROR %x\n",
                   *(volatile u32*)(0x1F801088 + (id << 4)));
            break;
        }
    }
    if (arg5 == 1) {
        u8* q = (u8*)D_800B54AC;
        v = q[2];
        v |= 1 << id;
        q[2] = v;
    } else {
        u8* q = (u8*)D_800B54AC;
        v = q[2];
        v &= ~(1 << id);
        q[2] = v;
    }
    do {
        ret = *(volatile s32*)D_800B54AC;
    } while (0);
    temp2 = id << 2;
    temp2 = temp2 + 3;
    temp = 1;
    temp = temp << temp2;
    ptr = &((volatile DMA_CONTROL*)0x1F801080)[id].madr;
    temp2 = *D_800B54A8;
    temp2 |= temp;
    *D_800B54A8 = temp2;
    *ptr++ = (s32)addr;
    *ptr++ = (blocks << 16) | size;
    while (!(*D_800B5490 & 0x40)) {
    }
    *ptr = control;
    do {
        ret = *ptr;
    } while (0);
}
#else
void dma_execute(s32 id, void* addr, u32 blocks, u32 size, u32 control,
                 u_char arg5, u32 arg6) {
    CD_getsector(addr, size);
    if (arg5 == 1) {
        Psyz_KernelDmaComplete(DMA_CHANNEL_CD);
    }
}
#endif
