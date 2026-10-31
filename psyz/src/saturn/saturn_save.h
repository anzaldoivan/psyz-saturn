/* saturn_save.h -- PS1 memory card files (bu00:) on the Saturn internal backup
 * device through the BIOS backup library. Defines
 * psyz_open/close/lseek/read/write, my_firstfile, my_nextfile, my_erase,
 * my_format and _bu_init; this header adds only the non-PS1 calls. Sole caller
 * of the BIOS backup library; the reset guard comes from saturn_pad
 * (saturn_smpc_resdisa/resenab).
 */
#ifndef SATURN_SAVE_H
#define SATURN_SAVE_H

#include <bup.h>
#include <stdint.h>

#define SATURN_SAVE_MAX_BLOCKS 3 /* PS1 blocks per file (record buffer cap) */
#define SATURN_SAVE_FD 3         /* the one fd (one file open at a time) */

/* Adapter causes (saturn_save_cause); 1..8 are BIOS bup_status_t values. */
#define SATURN_SAVE_E_ARG 0x100     /* bad path, name, fd, mode or range */
#define SATURN_SAVE_E_RESDISA 0x101 /* reset guard timeout: no BIOS call */
#define SATURN_SAVE_E_FOREIGN 0x102 /* key holds a foreign record or name */
#define SATURN_SAVE_E_DIR 0x103     /* BUP_Dir table too small */
#define SATURN_SAVE_E_BUSY 0x104    /* a file is open */
#define SATURN_SAVE_E_CAP 0x105     /* over SATURN_SAVE_MAX_BLOCKS */

typedef void (*saturn_save_bios_init_t)(
    void* lib, void* work, bup_devices_t* devices);

/* BUP_Stat of device 0 for records of datasize bytes: the BIOS status (0 ok),
 * -1 when the backup library is unavailable (reset guard timeout). */
int saturn_save_stat(uint32_t datasize, bup_stat_t* out);

/* Tests only: the last latched cause, and the RESENAB timeouts so far. */
int saturn_save_cause(void);
int saturn_save_guard_open(void);

#endif
