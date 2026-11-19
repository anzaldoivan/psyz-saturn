
#ifndef LIBCD_PRIVATE_H
#define LIBCD_PRIVATE_H
#include <common.h>
#include <libcd.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int CD_debug;

#ifdef __psyz
int CD_getsector(void* madr, int size);
void Psyz_KernelDmaComplete(int channel);
#endif

#ifdef __cplusplus
}
#endif

#endif
