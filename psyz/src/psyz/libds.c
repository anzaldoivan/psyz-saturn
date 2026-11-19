#include <psyz.h>
#include <libds.h>
#include "../../../decomp/src/libcd/libcd_private.h"

int DsGetSector(void* madr, int size) { return CD_getsector(madr, size) == 0; }
