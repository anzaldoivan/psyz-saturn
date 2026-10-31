/* saturn_movie.h -- movie service: one
 * blocking call plays a Cinepak FILM from the disc through the CD owner
 * (saturn_cd), the display seam (saturn_gpu.c) and the sound seam
 * (saturn_spu.c), paced by a VBlank schedule of 4 VBlanks per frame. Reached
 * only from the title's call (saturn.mk PSYZ_SATURN_MOVIE_SRCS). */
#ifndef SATURN_MOVIE_H
#define SATURN_MOVIE_H

#include <stdint.h>

/* saturn_movie_play statuses */
#define SATURN_MOVIE_END 0
#define SATURN_MOVIE_GUARD 1      /* decoder status 1 or 2 (malformed chunk) */
#define SATURN_MOVIE_BAD_HEADER 2 /* magic, version, sample count, size */
#define SATURN_MOVIE_NOT_FOUND 3  /* CdSearchFile failed */
#define SATURN_MOVIE_CD_REJECT 4  /* start_read refused twice for one unit */
#define SATURN_MOVIE_CD_TIMEOUT 5 /* owner SATCD_ERROR by its age bound */
#define SATURN_MOVIE_CD_ERROR 6   /* owner SATCD_ERROR by a hardware error */
#define SATURN_MOVIE_UNDERRUN 7   /* bytes not landed for 120 VBlanks */
#define SATURN_MOVIE_DISPLAY_REFUSED 8 /* saturn_gpu_movie_begin -1 */
#define SATURN_MOVIE_DMA_PENDING 9     /* saturn_gpu_movie_frame nonzero */

typedef struct {
    int status;
    uint32_t frames;          /* frames enqueued and shown */
    uint32_t late_frames;     /* enqueued after their due VBlank, not dropped */
    uint32_t max_late;        /* VBlanks */
    uint32_t polls;           /* the service's saturn_cd_poll calls */
    uint32_t bytes_read;      /* sectors landed x 2048 */
    uint32_t underrun_frames; /* VBlanks waited for bytes after the prefill */
    uint32_t cd_rejects, cd_timeouts, cd_errors; /* saturn_cd_stats deltas */
    uint32_t sound_refused; /* 1: saturn_spu_movie_begin refused, silent */
    uint32_t dropped; /* decoded a frame period or more past due, not shown */
} saturn_movie_stats_t;

int saturn_movie_play(const char* iso_path, saturn_movie_stats_t* out);

/* Per-frame log entry, filled at each frame k < n, shown or dropped
 * (saturn_movie_log_set); VBlanks are the service's counted VBlanks since the
 * play began. */
typedef struct {
    uint32_t k;               /* frame index; bit 31 set: dropped */
    uint32_t due;             /* due VBlank, v0 + 4k */
    uint32_t shown;           /* VBlank-in whose DMA shows it, written once
                                 that VBlank-in is counted; dropped: the
                                 VBlank it was dropped at */
    uint32_t sectors;         /* sectors landed, cumulative */
    uint32_t lead;            /* ring lead bytes at enqueue, landed end - pos */
    uint32_t latency;         /* max issue->land VBlanks of units landed since
                                 the previous entry */
    uint32_t polls;           /* stats.polls, cumulative */
    uint32_t underrun_frames; /* stats.underrun_frames, cumulative */
} saturn_movie_frame_t;

/* log NULL (the default) = no writes; kept across plays until set again. */
void saturn_movie_log_set(saturn_movie_frame_t* log, uint32_t n);

/* Latched VCNT (EXTEN read, then VCNT); defined by saturn_misc.c. */
uint16_t saturn_frame_vcount(void);

/* Display seam, defined by saturn_gpu.c: begin -1 = refused (display off);
 * frame nonzero = the previous frame's DMA still pending; vblanks = VBlank-ins
 * since begin, counted by libyaul's VBlank-in callback (the movie clock). */
int saturn_gpu_movie_begin(void);
int saturn_gpu_movie_frame(const uint16_t* buf);
uint32_t saturn_gpu_movie_vblanks(void);
void saturn_gpu_movie_end(void);
/* Sound seam, defined by saturn_spu.c: begin -1 = refused (silent movie);
 * lead = bytes written ahead of the play block, -1 when off or no slot. */
int saturn_spu_movie_begin(uint32_t rate);
void saturn_spu_movie_write(const uint8_t* pcm, uint32_t n);
int saturn_spu_movie_lead(void);
void saturn_spu_movie_end(void);

#endif
