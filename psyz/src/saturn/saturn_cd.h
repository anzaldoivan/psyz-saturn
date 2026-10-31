/* saturn_cd.h -- Saturn CD owner: sole user of the CD hardware port.
 * One op in flight; every wait bounded (SATCD_WAIT_BOUND polls); LBA<->FAD bias
 * applied in one place; a FAD outside [first track, lead-out) is rejected
 * before any hardware call. The PSY-Q libcd/libds facade (saturn_libcd.c) is
 * the only intended client. */
#ifndef SATURN_CD_H
#define SATURN_CD_H

#include <stdint.h>

#define SATCD_SECTOR 2048
#define SATCD_FAD_BIAS                                                         \
    150 /* LBA 0 = FAD 150 (2 s pregap). not verified from official manual */
#define SATCD_WAIT_BOUND                                                       \
    1200 /* frame-paced polls, as PSY-Q's 0x4B0 VSyncs, not                    \
            register exchanges. not verified from official manual */
#define SATCD_MAX_TRACKS 100

enum { SATCD_IDLE, SATCD_BUSY, SATCD_DONE, SATCD_ERROR };

/* ---- hardware port: implemented by the platform glue (libyaul CD block) or by
 * the host fake ---- */
enum { SATCD_HW_BUSY, SATCD_HW_SECTOR, SATCD_HW_DONE, SATCD_HW_ERROR };
int saturn_cdhw_toc(
    uint32_t* fad, int max, uint32_t* leadout); /* track count; <= 0: no disc */
int saturn_cdhw_seek(uint32_t fad);             /* issue; 0 = accepted */
int saturn_cdhw_play(
    uint32_t fad, uint32_t n); /* play n sectors, open the cursor; 0 = ok */
/* issue n-sector read (at an open play's cursor: no command); 0 = accepted */
int saturn_cdhw_read(uint32_t fad, uint32_t n);
int saturn_cdhw_poll(void); /* one tick: SATCD_HW_* */
uint32_t saturn_cdhw_take(
    void* dst); /* copy the sectors SATCD_HW_SECTOR announced; count */
void saturn_cdhw_abort(void);

/* ---- owner ---- */
struct saturn_cd_stats {
    uint32_t rejects;  /* refused before any hardware call (out-of-image FAD,
                          busy, no disc) */
    uint32_t timeouts; /* ops aborted by the age bound */
    uint32_t errors;   /* ops ended by a hardware error */
    uint32_t bful;     /* CD buffer full seen at poll */
};
extern struct saturn_cd_stats saturn_cd_stats;

int saturn_cd_init(void); /* reads the TOC; track count, 0 = no disc */
int saturn_cd_tracks(uint32_t* fad, int max);
uint32_t saturn_cd_leadout(void); /* lead-out FAD; 0 = no disc */
int saturn_cd_fad_valid(uint32_t fad, uint32_t n);
uint32_t saturn_cd_lba_to_fad(int32_t lba);
int32_t saturn_cd_fad_to_lba(uint32_t fad);
int saturn_cd_start_read(uint32_t fad, uint32_t n, void* dst); /* 1 = started */
int saturn_cd_start_seek(uint32_t fad);                        /* 1 = started */
/* one play over n sectors; reads at its cursor continue it; 1 = opened */
int saturn_cd_play_open(uint32_t fad, uint32_t n);
int saturn_cd_poll(void);        /* exactly one hardware poll while busy */
int saturn_cd_wait(void);        /* bounded; SATCD_DONE or SATCD_ERROR */
void saturn_cd_frame_wait(void); /* one frame between saturn_cd_wait polls;
                                    weak no-op default */
int saturn_cd_busy(void);
uint32_t saturn_cd_remaining(void);
uint32_t saturn_cd_done_sectors(void);
void saturn_cd_abort(void);
int saturn_cd_read_sync(
    uint32_t fad, uint32_t n, void* dst); /* 1 = read completed */

/* ---- facade diagnostics: cause of the last DsSearchFile failure (it returns 0
 * for all of them) ---- */
enum {
    SATDS_OK,
    SATDS_PATH,
    SATDS_BUSY,
    SATDS_IO,
    SATDS_NOPVD,
    SATDS_MALFORMED,
    SATDS_NOTFOUND
};
extern int saturn_ds_err;
/* LibRef47 p0700; not declared in PsyZ libcd.h. */
void CdReadBreak(void);
/* Called once per VSync (saturn_misc.c Psyz_VideoVSync): nothing before
 * CdInit; one saturn_cd_poll() while an op is in flight; fires the libcd read
 * and sync callbacks (they never fire elsewhere). */
void saturn_cd_pump(void);

#endif
