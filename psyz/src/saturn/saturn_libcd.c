/* saturn_libcd.c -- PSY-Q libcd/libds facade over the Saturn CD owner
 * (saturn_cd.c). Types and constants: PsyZ include/libcd.h, libds.h. Contracts:
 * LibRef 4.7 (LibRef47) pages cited per function; "N" = not verified from
 * official manual.
 * Callbacks fire only from saturn_cd_pump() (Psyz_VideoVSync): a terminal
 * result is latched where it is seen and fired by the next pump. */
#include <libcd.h>
#include <libds.h>

#include "saturn_cd.h"

#include <ctype.h>
#include <string.h>

#define MODE_SIZE_MASK                                                         \
    (CdlModeSize1 | CdlModeSize0) /* 2340/2328-byte reads unsupported */
#define SLICE 32 /* sectors per read op while a music stream is on */

int saturn_ds_err;

static struct {
    uint32_t loc; /* Setloc target, FAD */
    int loc_valid;
    uint32_t head; /* last seek/play/read FAD */
    int mode;
    int sync; /* status of the last non-read command */
    int read_active;
    int read_err; /* one-shot -1 for the next CdReadSync */
    int playing;
    int inited;     /* CdInit ran: the pump may poll */
    int read_latch; /* CdlComplete / CdlDiskError for the read callback */
    int sync_latch; /* same for the sync callback */
    int tracks;     /* TOC track count at CdInit, 0 = no disc */
    u_char resp[8]; /* last served command's response, for CdSync */
    int resp_len;
    CdlFILTER filter; /* CdlSetfilter: file, chan */
    /* a read queued beside the music stream: next FAD, sectors left, done,
     * slice in flight */
    u_char* qbuf;
    uint32_t qfad, qleft, qdone, qslice;
    int q;
} cd;

static CdlCB read_cb, sync_cb;
static u_char cb_result[8]; /* result handed to callbacks: not modelled (N) */

static uint32_t secbuf[SATCD_SECTOR / 4];

/* music hooks (saturn_music.h); saturn_music.c (opt-in) overrides them */
__attribute__((weak)) int saturn_music_read(uint32_t fad, int chan) {
    (void)fad;
    (void)chan;
    return 0;
}
__attribute__((weak)) void saturn_music_end(void) {}
__attribute__((weak)) int saturn_music_on(void) { return 0; }
__attribute__((weak)) void saturn_music_step(void) {}
__attribute__((weak)) void saturn_music_slice(void) {}

static int bcd_ok(u_char b, int max) {
    return (b & 15) <= 9 && (b >> 4) <= 9 && (b >> 4) * 10 + (b & 15) < max;
}
static int unbcd(u_char b) { return (b >> 4) * 10 + (b & 15); }
static u_char bcd(uint32_t v) { return (u_char)((v / 10) << 4 | v % 10); }

/* CdlLOC is absolute MSF (BCD); MSF sector count == FAD (LibRef47 p0674
 * CdlLOC). */
static void fad_to_loc(uint32_t fad, u_char* p) {
    p[0] = bcd(fad / 4500);
    p[1] = bcd(fad / 75 % 60);
    p[2] = bcd(fad % 75);
    p[3] = 0;
}

static int loc_to_fad(const u_char* p, uint32_t* fad) {
    if (!bcd_ok(p[0], 100) || !bcd_ok(p[1], 60) || !bcd_ok(p[2], 75))
        return 0;
    *fad = (uint32_t)((unbcd(p[0]) * 60 + unbcd(p[1])) * 75 + unbcd(p[2]));
    return 1;
}

/* LibRef47 p0689: 1 on success, 0 on failure (no disc). */
int CdInit(void) {
    int n = saturn_cd_init();
    memset(&cd, 0, sizeof cd);
    cd.head = saturn_cd_lba_to_fad(0);
    cd.sync = n ? CdlComplete : CdlDiskError;
    cd.inited = 1;
    cd.tracks = n > 0 ? n : 0;
    return n > 0;
}

/* terminal results: latched here, fired by saturn_cd_pump() */
static void sync_end(int st) { cd.sync = cd.sync_latch = st; }
static void read_end(int ok) {
    cd.read_active = 0;
    cd.read_latch = ok ? CdlComplete : CdlDiskError;
}

/* LibRef47 p0691 / p0697: absolute sector number <-> time code. The +150 pregap
 * is N (owner's one bias). */
CdlLOC* CdIntToPos(int i, CdlLOC* p) {
    fad_to_loc(saturn_cd_lba_to_fad(i), (u_char*)p);
    return p;
}

int CdPosToInt(CdlLOC* p) {
    uint32_t fad = 0;
    loc_to_fad((u_char*)p, &fad);
    return saturn_cd_fad_to_lba(fad);
}

/* One op in flight: while busy only CdlNop and CdlGetlocP are served (N).
 * While a music stream is on no command needs the drive: none is refused. */
static int refused(u_char com) {
    return saturn_cd_busy() && !saturn_music_on() && com != CdlNop &&
           com != CdlGetlocP;
}

/* A served command's response {stat, ...} is cached for CdSync and copied to
 * result when non-NULL. */
static void reply(const u_char* r, int len, u_char* result) {
    memcpy(cd.resp, r, (size_t)len);
    cd.resp_len = len;
    if (result)
        memcpy(result, r, (size_t)len);
}

/* GetTN / GetTD with no disc, a bad BCD track or a track past the last: error
 * response {stat | CdlStatError, 0x80, 0, 0}, sync CdlDiskError (shape of
 * upstream PsyZ test_libcd.c no_disk_loaded / invalid_track_number; N). */
static int toc_fail(u_char* r, u_char* result) {
    r[0] |= CdlStatError;
    r[1] = 0x80;
    sync_end(CdlDiskError);
    reply(r, 4, result);
    return 0;
}

/* LibRef47 p0677-0678: 1 if issued, 0 if failed; command codes per PsyZ
 * libcd.h. Status byte: CdlStatStandby while a disc is present (PS1 constant;
 * Saturn mapping N). */
static int issue(u_char com, u_char* param, u_char* result) {
    uint32_t fad, toc[SATCD_MAX_TRACKS];
    u_char r[8] = {0};
    int len = 4, t;
    r[0] = cd.tracks ? CdlStatStandby : 0;
    switch (com) {
    case CdlSetloc:
        /* Out-of-image or malformed MSF is refused here, so no later read can
         * use it. */
        if (param == 0 || !loc_to_fad(param, &fad) ||
            !saturn_cd_fad_valid(fad, 1)) {
            cd.loc_valid = 0;
            saturn_cd_stats.rejects++;
            return 0;
        }
        cd.loc = fad; /* CdlLOC is already absolute: no second +150 */
        cd.loc_valid = 1;
        break;
    case CdlSetmode: /* 2340/2328-byte modes refused, mode unchanged */
        if (param == 0 || (param[0] & MODE_SIZE_MASK))
            return 0;
        cd.mode = param[0];
        break;
    case CdlNop:
        break;
    case CdlSetfilter: /* CdlFILTER (LibRef47 p0673) */
        if (param == 0)
            return 0;
        memcpy(&cd.filter, param, sizeof cd.filter);
        break;
    case CdlPause:
    case CdlStop:
        cd.playing = 0;
        saturn_music_end();
        break;
    case CdlReadN:
    case CdlReadS:
        /* ADPCM (CdlModeRT) reads play the title's music map (saturn_music.c)
         * from the CdlLOC, else the last Setloc, else the head; no other read
         * mode is streamed. */
        if (param) {
            if (!loc_to_fad(param, &fad))
                return 0;
            cd.loc = fad;
            cd.loc_valid = 1;
        } else {
            fad = cd.loc_valid ? cd.loc : cd.head;
        }
        if (!(cd.mode & CdlModeRT) ||
            !saturn_music_read(
                fad, cd.mode & CdlModeSF ? (int)cd.filter.chan : -1))
            return 0;
        cd.head = fad;
        break;
    case CdlGetlocP: /* track, index, rel m:s:f, abs m:s:f (rel reported as abs:
                        N) */
    {
        u_char p[4];
        fad_to_loc(cd.head, p);
        r[0] = 1, r[1] = 1;
        memcpy(r + 2, p, 3);
        memcpy(r + 5, p, 3);
        len = 8;
        break;
    }
    case CdlGetTN: /* stat, BCD first track, BCD last track */
        if (!cd.tracks)
            return toc_fail(r, result);
        r[1] = bcd(1);
        r[2] = bcd((uint32_t)cd.tracks);
        break;
    case CdlGetTD: /* BCD track (0 = lead-out) -> stat, BCD min, BCD sec */
        if (!cd.tracks || param == 0 || !bcd_ok(param[0], 100) ||
            (t = unbcd(param[0])) > cd.tracks)
            return toc_fail(r, result);
        saturn_cd_tracks(toc, SATCD_MAX_TRACKS);
        fad = t ? toc[t - 1] : saturn_cd_leadout();
        { /* absolute MSF of the FAD, as CdlLOC */
            u_char p[4];
            fad_to_loc(fad, p);
            r[1] = p[0], r[2] = p[1];
        }
        break;
    case CdlSeekL:
    case CdlSeekP:
    case CdlPlay:
        fad = cd.loc_valid ? cd.loc : cd.head;
        if (saturn_music_on()) { /* the stream holds the drive: at once */
            cd.head = fad;
            cd.playing = com == CdlPlay;
            break;
        }
        if (!saturn_cd_start_seek(fad)) {
            sync_end(CdlDiskError);
            return 0;
        }
        cd.head = fad;
        cd.sync = CdlNoIntr;
        cd.playing = com == CdlPlay;
        reply(r, len, result);
        return 1;
    default:
        return 0;
    }
    sync_end(CdlComplete);
    reply(r, len, result);
    return 1;
}

int CdControl(u_char com, u_char* param, u_char* result) {
    if (refused(com))
        return 0;
    return issue(com, param, result);
}

/* PsyZ libcd.h:397: CdControl, then a bounded CdSync(0, result); 1 iff the
 * command completed (CdlComplete), else 0 (contract N). */
int CdControlB(u_char com, u_char* param, u_char* result) {
    if (!CdControl(com, param, result))
        return 0;
    return CdSync(0, result) == CdlComplete;
}

/* LibRef47 p0680: no handshake, always returns 1. */
int CdControlF(u_char com, u_char* param) {
    if (!refused(com))
        issue(com, param, 0);
    return 1;
}

/* LibRef47 p0711: mode 0 waits (bounded here), mode 1 returns now; CdlComplete
 * / CdlDiskError / CdlNoIntr. result gets the last response, its own length
 * only. */
/* One poll of the drive shared with a music stream: the stream first (it
 * starts its next unit when the owner is idle and its cover is low), then the
 * queued read's next slice, landing in order in the caller's buffer. */
static void drive_step(void) {
    if (saturn_cd_busy())
        saturn_cd_poll();
    if (cd.qslice && !saturn_cd_busy()) {
        if (saturn_cd_poll() == SATCD_DONE) { /* idle: the slice's end */
            cd.qbuf += cd.qslice * SATCD_SECTOR;
            cd.qfad += cd.qslice;
            cd.qleft -= cd.qslice;
            cd.qdone += cd.qslice;
        } else {
            cd.read_err = 1; /* one-shot -1 for CdReadSync */
            cd.q = 0;
            read_end(0);
        }
        cd.qslice = 0;
        if (cd.q && cd.qleft == 0) {
            cd.q = 0;
            read_end(1);
        }
    }
    saturn_music_step();
    if (cd.q && !saturn_cd_busy()) {
        uint32_t n = cd.qleft < SLICE ? cd.qleft : SLICE;
        if (saturn_cd_start_read(cd.qfad, n, cd.qbuf)) {
            cd.qslice = n;
            saturn_music_slice();
        }
    }
}

int CdSync(int mode, u_char* result) {
    if (cd.q || saturn_music_on())
        drive_step();
    if (cd.sync == CdlNoIntr && !cd.read_active) {
        int st = mode == 0 ? saturn_cd_wait() : saturn_cd_poll();
        if (st != SATCD_BUSY)
            sync_end(st == SATCD_DONE ? CdlComplete : CdlDiskError);
    }
    if (result && cd.resp_len)
        memcpy(result, cd.resp, (size_t)cd.resp_len);
    return cd.sync;
}

/* LibRef47 p0698: reads from the last Setloc; 1 if issued, else 0. A refusal
 * with no read in flight arms a one-shot -1 for CdReadSync, so a caller that
 * ignores this return never sees 0. */
int CdRead(int sectors, u_long* buf, int mode) {
    int q = saturn_music_on() != 0; /* queued: drive_step slices it */
    if (cd.read_active)
        return 0;
    if ((mode & MODE_SIZE_MASK) || !cd.loc_valid || sectors <= 0 || buf == 0 ||
        !(q ? saturn_cd_fad_valid(cd.loc, (uint32_t)sectors)
            : saturn_cd_start_read(cd.loc, (uint32_t)sectors, buf))) {
        cd.read_err = 1;
        return 0;
    }
    cd.q = q;
    cd.qbuf = (u_char*)buf;
    cd.qfad = cd.loc;
    cd.qleft = (uint32_t)sectors;
    cd.qdone = cd.qslice = 0;
    cd.head = cd.loc;
    cd.read_active = 1;
    cd.read_err = 0;
    return 1;
}

/* LibRef47 p0704: sectors remaining, 0 complete, -1 error; mode 1 returns at
 * once; the 1200-poll age bound is PSY-Q's 0x4B0 VSyncs. */
int CdReadSync(int mode, u_char* result) {
    int st;
    uint32_t idle = 0;
    (void)result;
    while (cd.q) { /* a queued read: one drive step per frame */
        drive_step();
        if (!cd.q || mode)
            break;
        if (!saturn_cd_busy() && ++idle >= SATCD_WAIT_BOUND) {
            cd.q = 0; /* no slice could start: bounded as the owner's wait */
            cd.read_err = 1;
            read_end(0);
            break;
        }
        saturn_cd_frame_wait();
    }
    if (cd.q)
        return (int)(cd.qleft - (cd.qslice ? saturn_cd_done_sectors() : 0));
    if (!cd.read_active) {
        if (cd.read_err) {
            cd.read_err = 0;
            return -1;
        }
        return 0;
    }
    st = mode == 0 ? saturn_cd_wait() : saturn_cd_poll();
    if (st == SATCD_BUSY)
        return (int)saturn_cd_remaining();
    read_end(st == SATCD_DONE);
    return st == SATCD_DONE ? 0 : -1;
}

/* LibRef47 p0701 / p0712: func(CdlComplete | CdlDiskError, result); returns
 * the previous callback. Fired from saturn_cd_pump() only. */
CdlCB CdReadCallback(CdlCB func) {
    CdlCB old = read_cb;
    read_cb = func;
    return old;
}

CdlCB CdSyncCallback(CdlCB func) {
    CdlCB old = sync_cb;
    sync_cb = func;
    return old;
}

/* Once per VSync: one owner poll while an op is in flight, settling a finished
 * read or seek so a callback-only client is never stuck; then fires the
 * latched results, each cleared before its callback runs. */
void saturn_cd_pump(void) {
    int st;
    if (!cd.inited)
        return;
    if (cd.q || saturn_music_on()) {
        drive_step();
    } else if (saturn_cd_busy()) {
        st = saturn_cd_poll();
        if (st != SATCD_BUSY && cd.read_active) {
            cd.read_err = st != SATCD_DONE; /* one-shot -1 for CdReadSync */
            read_end(st == SATCD_DONE);
        } else if (st != SATCD_BUSY && cd.sync == CdlNoIntr) {
            sync_end(st == SATCD_DONE ? CdlComplete : CdlDiskError);
        }
    }
    if (cd.read_latch) {
        st = cd.read_latch;
        cd.read_latch = 0;
        if (read_cb)
            read_cb((u_char)st, cb_result);
    }
    if (cd.sync_latch) {
        st = cd.sync_latch;
        cd.sync_latch = 0;
        if (sync_cb)
            sync_cb((u_char)st, cb_result);
    }
}

/* LibRef47 p0700: a break before one sector "can cause an error": modelled as
 * one -1 from CdReadSync (N). */
void CdReadBreak(void) {
    if (cd.read_active) {
        uint32_t n = cd.q ? cd.qdone : 0;
        if (!cd.q || cd.qslice) { /* not a music op */
            n += saturn_cd_done_sectors();
            saturn_cd_abort();
        }
        cd.read_err = n == 0;
        cd.read_active = cd.q = 0;
        cd.qslice = 0;
    }
}

/* LibRef47 p0688: positive = track count, anything else an error. toc[i] =
 * start of track i+1 (N). */
int CdGetToc(CdlLOC* loc) {
    uint32_t fad[SATCD_MAX_TRACKS];
    int i, n = saturn_cd_tracks(fad, SATCD_MAX_TRACKS);
    for (i = 0; i < n; i++)
        fad_to_loc(fad[i], (u_char*)&loc[i]);
    return n;
}

/* ---------------- libds ---------------- */
static uint32_t le32(const u_char* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static DslFILE* ds_fail(int cause) {
    /* LibRef47 p0769 names -1 for a failed directory read, but the shipped
     * implementation returns 0 on every failure: visible 0, cause kept. */
    saturn_ds_err = cause;
    return 0;
}

/* Scan every sector of one directory; a malformed record invalidates the whole
 * directory. */
static int search_dir(uint32_t lba, uint32_t size, const char* want, size_t wl,
                      int want_dir, uint32_t* out_lba, uint32_t* out_size) {
    const u_char* s = (const u_char*)secbuf;
    uint32_t k, nsec = (size + SATCD_SECTOR - 1) / SATCD_SECTOR;
    int found = 0;
    for (k = 0; k < nsec; k++) {
        uint32_t i = 0;
        if (!saturn_cd_read_sync(
                saturn_cd_lba_to_fad((int32_t)(lba + k)), 1, secbuf))
            return SATDS_IO;
        while (i < SATCD_SECTOR &&
               s[i] != 0) { /* zero length: rest of sector is padding */
            uint32_t L = s[i], nl = i + 32 < SATCD_SECTOR ? s[i + 32] : 0, n;
            if (L < 33 + nl || nl == 0 || i + L > SATCD_SECTOR) {
                return SATDS_MALFORMED;
            }
            n = nl;
            {
                const void* semi = memchr(s + i + 33, ';', nl);
                if (semi)
                    n = (uint32_t)((const u_char*)semi - (s + i + 33));
            }
            if (!found && n == wl &&
                ((s[i + 25] & 2) != 0) == (want_dir != 0)) {
                uint32_t j = 0;
                while (
                    j < n && toupper(s[i + 33 + j]) == toupper((u_char)want[j]))
                    j++;
                if (j == n) {
                    found = 1;
                    *out_lba = le32(s + i + 2);
                    *out_size = le32(s + i + 10);
                }
            }
            i += L;
        }
    }
    return found ? SATDS_OK : SATDS_NOTFOUND;
}

/* ISO9660 search shared by DsSearchFile and CdSearchFile: SATDS_OK, else the
 * cause. On success pos (absolute MSF), *size and name[16] are filled. */
static int iso_search(
    char* name, u_char* pos, uint32_t* out_size, char* fname) {
    const u_char* s = (const u_char*)secbuf;
    const char *p, *e;
    uint32_t lba, size;
    int parts = 0, r;

    if (pos == 0 || name == 0 || name[0] != '\\')
        return SATDS_PATH;
    for (p = name + 1;;
         p = e + 1) { /* path check before any I/O; <= 8 levels */
        e = strchr(p, '\\');
        if (++parts > 8 || (e ? e : p + strlen(p)) == p)
            return SATDS_PATH;
        if (!e)
            break;
    }
    if (saturn_cd_busy())
        return SATDS_BUSY;
    if (!saturn_cd_read_sync(saturn_cd_lba_to_fad(16), 1, secbuf))
        return SATDS_IO;
    if (memcmp(s, "\001CD001", 6) != 0)
        return SATDS_NOPVD;
    lba = le32(s + 156 + 2);
    size = le32(s + 156 + 10);
    for (p = name + 1;; p = e + 1) {
        const char* semi;
        size_t wl;
        e = strchr(p, '\\');
        wl = e ? (size_t)(e - p) : strlen(p);
        semi = memchr(p, ';', wl);
        if (semi)
            wl = (size_t)(semi - p);
        r = search_dir(lba, size, p, wl, e != 0, &lba, &size);
        if (r != SATDS_OK)
            return r;
        if (!e)
            break;
    }
    CdIntToPos((int)lba, (CdlLOC*)pos);
    *out_size = size;
    memset(fname, 0, 16);
    memcpy(fname, p, strlen(p) < 15 ? strlen(p) : 15);
    return SATDS_OK;
}

/* LibRef47 p0769: absolute path; returns fp, 0 when not found (and, here, on
 * every other failure). */
DslFILE* DsSearchFile(DslFILE* fp, char* name) {
    uint32_t size;
    int r =
        iso_search(name, fp ? (u_char*)&fp->pos : 0, &size, fp ? fp->name : 0);
    saturn_ds_err = SATDS_OK;
    if (r != SATDS_OK)
        return ds_fail(r);
    fp->size = size;
    return fp;
}

/* LibRef47 p0708: fp, 0 if not found; 0 on every other failure too (cause in
 * saturn_ds_err; no directory cache: each call reads the PVD). */
CdlFILE* CdSearchFile(CdlFILE* fp, char* name) {
    uint32_t size;
    saturn_ds_err =
        iso_search(name, fp ? (u_char*)&fp->pos : 0, &size, fp ? fp->name : 0);
    if (saturn_ds_err != SATDS_OK)
        return 0;
    fp->size = (int)size;
    return fp;
}
