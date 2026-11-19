#include "ztest.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psyz.h>
#include <libapi.h>
#include <libcd.h>
#include <libds.h>
#include <libetc.h>
#include <libpress.h>
#include <libspu.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

enum { SECTOR_SIZE = 2352 };
enum { PATH_LEN = 256, MAX_BINS = 16, MAX_ALLOCS = 4 };

static char dir[PATH_LEN];
static char cue[PATH_LEN];
static char bins[MAX_BINS][PATH_LEN];
static int bin_count;
static void* allocs[MAX_ALLOCS];
static int alloc_count;

static void make_blob(const char* path, long bytes) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        zprintf("fopen %s\n", path);
    }
    zassert_ptr_ne(NULL, f);
    if (bytes > 0) {
        fseek(f, bytes - 1, SEEK_SET);
        fputc(0, f);
    }
    fclose(f);
}

static void write_text(const char* path, const char* body) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        zprintf("fopen %s\n", path);
    }
    zassert_ptr_ne(NULL, f);
    fwrite(body, 1, strlen(body), f);
    fclose(f);
}

static void append(char* buf, size_t cap, const char* fmt, ...) {
    size_t len = strlen(buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

static void add_bin(const char* path) {
    zassert_s32_gt(bin_count, MAX_BINS);
    snprintf(bins[bin_count++], PATH_LEN, "%s", path);
}

// Released in the teardown so that a zassert does not leak the buffer.
static void* test_calloc(size_t count, size_t size) {
    void* p;
    zassert_s32_gt(alloc_count, MAX_ALLOCS);
    p = calloc(count, size);
    zassert_ptr_ne(NULL, p);
    allocs[alloc_count++] = p;
    return p;
}

static void mount_cue(void) {
    int ret = Psyz_CdSetDiskPath(cue);
    if (ret != 0) {
        zprintf("Psyz_CdSetDiskPath failed for %s\n", cue);
    }
    zassert_s32_eq(0, ret);
    Psyz_CdShellOpen(0);
}

static void reset_state(const char* prefix) {
    snprintf(dir, sizeof(dir), "%s%lld", prefix, (long long)time(NULL));
    cue[0] = '\0';
    bin_count = 0;
    alloc_count = 0;
}

static void cleanup_files(void) {
    int i;
    for (i = 0; i < bin_count; i++) {
        remove(bins[i]);
    }
    if (cue[0] != '\0') {
        remove(cue);
    }
    rmdir(dir);
    for (i = 0; i < alloc_count; i++) {
        free(allocs[i]);
        allocs[i] = NULL;
    }
    alloc_count = 0;
}

static int g_read_cb_calls = 0;
static int g_read_cb_fails = 0;
static unsigned int g_read_cb_last_sector = 0;
static unsigned int g_read_cb_last_size = 0;

static int fake_read_cb(struct PsyzCdRead* read) {
    unsigned char* b;
    unsigned int i;
    g_read_cb_calls++;
    g_read_cb_last_sector = read->sector;
    g_read_cb_last_size = read->size;
    if (g_read_cb_fails) {
        return -1;
    }
    b = (unsigned char*)read->buffer;
    for (i = 0; i < read->size; ++i) {
        b[i] = (unsigned char)(0xC0 + i + read->sector);
    }
    return (int)read->size;
}

// Build a CUE describing N MODE2/2352 + audio tracks all sharing one BIN.
// bin_sectors lets the caller fix the lead-out MSF.
static void load_cue_single(int track_count, long bin_sectors, bool has_data) {
    char bin[PATH_LEN];
    char body[1024] = "FILE \"disc.bin\" BINARY\n";
    int i;
    snprintf(bin, sizeof(bin), "%s/disc.bin", dir);
    make_blob(bin, bin_sectors * SECTOR_SIZE);
    add_bin(bin);

    for (i = 1; i <= track_count; ++i) {
        const char* type = "AUDIO";
        if (i == 1 && has_data) {
            type = "MODE2/2352";
        }
        append(body, sizeof(body), "  TRACK %02d %s\n", i, type);
        append(body, sizeof(body), "    INDEX 01 00:00:00\n");
    }

    snprintf(cue, sizeof(cue), "%s/disc.cue", dir);
    write_text(cue, body);
    mount_cue();
}

// CUE with one MODE2/2352 data track + N audio tracks each in its own BIN
static void load_cue_multi(
    int audio_track_count, long data_sectors, long audio_sectors_each) {
    char body[2048] = "";
    char data_bin[PATH_LEN];
    int i;
    snprintf(data_bin, sizeof(data_bin), "%s/data.bin", dir);
    make_blob(data_bin, data_sectors * SECTOR_SIZE);
    add_bin(data_bin);
    append(body, sizeof(body), "FILE \"data.bin\" BINARY\n");
    append(body, sizeof(body), "  TRACK 01 MODE2/2352\n");
    append(body, sizeof(body), "    INDEX 01 00:00:00\n");

    for (i = 0; i < audio_track_count; ++i) {
        char fname[64];
        char ab[PATH_LEN];
        snprintf(fname, sizeof(fname), "audio%02d.bin", i + 1);
        snprintf(ab, sizeof(ab), "%s/%s", dir, fname);
        make_blob(ab, audio_sectors_each * SECTOR_SIZE);
        add_bin(ab);

        append(body, sizeof(body), "FILE \"%s\" BINARY\n", fname);
        append(body, sizeof(body), "  TRACK %02d AUDIO\n", i + 2);
        append(body, sizeof(body), "    INDEX 01 00:00:00\n");
    }

    snprintf(cue, sizeof(cue), "%s/disc.cue", dir);
    write_text(cue, body);
    mount_cue();
}

static void build_data_sector(unsigned char* sector, int sector_idx) {
    int i;
    memset(sector, 0, SECTOR_SIZE);
    sector[0] = 0x00;
    for (i = 1; i < 11; ++i) {
        sector[i] = 0xFF;
    }
    sector[11] = 0x00;
    for (i = 12; i < 24; ++i) {
        sector[i] = (unsigned char)(0xA0 + i + sector_idx);
    }
    for (i = 24; i < SECTOR_SIZE; ++i) {
        sector[i] = (unsigned char)(sector_idx * 31 + i);
    }
}

static void load_cue_sectors(
    int sectors, void (*build)(unsigned char* sector, int sector_idx)) {
    char bin[PATH_LEN];
    unsigned char sector[SECTOR_SIZE];
    FILE* f;
    int i;
    snprintf(bin, sizeof(bin), "%s/data.bin", dir);
    f = fopen(bin, "wb");
    if (f == NULL) {
        zprintf("fopen %s\n", bin);
    }
    zassert_ptr_ne(NULL, f);
    for (i = 0; i < sectors; ++i) {
        build(sector, i);
        zassert_u32_eq(SECTOR_SIZE, fwrite(sector, 1, SECTOR_SIZE, f));
    }
    fclose(f);
    add_bin(bin);

    snprintf(cue, sizeof(cue), "%s/data.cue", dir);
    write_text(cue, "FILE \"data.bin\" BINARY\n"
                    "  TRACK 01 MODE2/2352\n"
                    "    INDEX 01 00:00:00\n");
    mount_cue();
}

static void load_cue_data(int sectors) {
    load_cue_sectors(sectors, build_data_sector);
}

// ISO 9660 disc: PVD at 16, L path table at 18, root at 22, STR at 23, XA at 24
enum { ISO_SECTORS = 35, ISO_DAT = 26, ISO_BIN = 28 };
enum { ISO_BIN_SIZE = 2 * 2048 + 300 };

static void put_le32(unsigned char* p, int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned char* put_path(
    unsigned char* p, int lba, const char* name, int len) {
    p[0] = (unsigned char)len;
    put_le32(p + 2, lba);
    p[6] = 1;
    memcpy(p + 8, name, len);
    return p + 8 + len + len % 2;
}

static unsigned char* put_record(
    unsigned char* p, int lba, int size, const char* name, int len) {
    int n = 33 + len + (33 + len) % 2;
    p[0] = (unsigned char)n;
    put_le32(p + 2, lba);
    put_le32(p + 10, size);
    p[0x20] = (unsigned char)len;
    memcpy(p + 0x21, name, len);
    return p + n;
}

static unsigned char* put_dots(unsigned char* p, int lba, int parent) {
    p = put_record(p, lba, 2048, "\0", 1);
    return put_record(p, parent, 2048, "\1", 1);
}

static void build_iso_user(unsigned char* user, int lba) {
    unsigned char* p = user;
    int i;
    memset(user, 0, 2048);
    switch (lba) {
    case 16:
    case 17:
        user[0] = lba == 16 ? 1 : 0xFF;
        memcpy(user + 1, "CD001", 5);
        user[6] = 1;
        if (lba == 16) {
            put_le32(user + 140, 18);
        }
        break;
    case 18:
        p = put_path(p, 22, "\0", 1);
        p = put_path(p, 23, "STR", 3);
        put_path(p, 24, "XA", 2);
        break;
    case 22:
        p = put_dots(p, 22, 22);
        p = put_record(p, ISO_BIN, ISO_BIN_SIZE, "GAME_01.BIN;1", 13);
        p = put_record(p, ISO_DAT, 2 * 2048, "GAME_01.DAT;1", 13);
        p = put_record(p, 23, 2048, "STR", 3);
        put_record(p, 24, 2048, "XA", 2);
        break;
    case 23:
        p = put_dots(p, 23, 22);
        p = put_record(p, 31, 2048, "INTRO01.STR;1", 13);
        put_record(p, 32, 2048, "OPEN.STR;1", 10);
        break;
    case 24:
        p = put_dots(p, 24, 22);
        p = put_record(p, 33, 2048, "BGM.XA;1", 8);
        put_record(p, 34, 2048, "DEMO.XA;1", 9);
        break;
    default:
        for (i = 0; i < 2048; ++i) {
            user[i] = (unsigned char)(lba * 31 + i);
        }
        break;
    }
}

static unsigned char to_bcd(int n) {
    return (unsigned char)((n / 10) << 4 | n % 10);
}

// Mode 2 form 1: sync, BCD MSF header, subheader, 2048 bytes of user data
static void build_iso_sector(unsigned char* sector, int lba) {
    int msf = lba + 150;
    memset(sector, 0, SECTOR_SIZE);
    memset(sector + 1, 0xFF, 10);
    sector[12] = to_bcd(msf / 75 / 60);
    sector[13] = to_bcd(msf / 75 % 60);
    sector[14] = to_bcd(msf % 75);
    sector[15] = 2;
    sector[18] = 0x08;
    sector[22] = 0x08;
    build_iso_user(sector + 24, lba);
}

static int g_iso_reads[32];
static int g_iso_read_count;

static int log_read_cb(struct PsyzCdRead* read) {
    if (g_iso_read_count < (int)(sizeof(g_iso_reads) / sizeof(*g_iso_reads))) {
        g_iso_reads[g_iso_read_count] = (int)read->sector;
    }
    g_iso_read_count++;
    return -1;
}

static void start_read_at(int sector) {
    CdlLOC loc;
    CdIntToPos(sector, &loc);
    CdControlB(CdlSetloc, (u_char*)&loc, NULL);
    CdControlB(CdlReadN, NULL, NULL);
}

static void check_td(u_char bcd_track, u_char exp_min, u_char exp_sec) {
    u_char p = bcd_track;
    u_char rr[8];
    zprintf("track %d\n", (int)bcd_track);
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, rr));
    zexpect_u8_eq(CdlStatStandby, rr[0]);
    zexpect_u8_eq(exp_min, rr[1]);
    zexpect_u8_eq(exp_sec, rr[2]);
}

ZTEST_SETUP(libcd) {
    int ret;
    reset_state("psyz_libcd_test_");
    ret = mkdir(dir, 0755);
    if (ret != 0) {
        zprintf("%s\n", dir);
    }
    zassert_s32_eq(0, ret);
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
}

ZTEST_TEARDOWN(libcd) {
    CdReadyCallback(NULL);
    Psyz_CdSetReadCB(NULL);
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
    Psyz_AudioDestroy();
    cleanup_files();
}

// Mirrors the real-hardware capture: 1 MODE2/2352 track, 271,796 sectors.
ZTEST(libcd, single_data_track_matches_real_hardware) {
    const long kSectors = 271796; // 639,264,192 bytes / 2352
    u_char r[8];
    u_char sr[8];
    u_char p;
    load_cue_single(1, kSectors, true);

    memset(r, 0xCC, sizeof(r));
    zassert_s32_eq(CdlDataReady, CdControlB(CdlNop, NULL, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x00, r[2]);
    zexpect_u8_eq(0x00, r[3]);

    memset(r, 0xCC, sizeof(r));
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x01, r[1]);
    zexpect_u8_eq(0x01, r[2]);
    zexpect_u8_eq(0x00, r[3]);

    memset(sr, 0xCC, sizeof(sr));
    zexpect_s32_eq(CdlComplete, CdSync(1, sr));
    zexpect_u8_eq(CdlStatStandby, sr[0]);
    zexpect_u8_eq(0x01, sr[1]);
    zexpect_u8_eq(0x01, sr[2]);
    zexpect_u8_eq(0x00, r[3]);

    // GetTD 01 -> 00:02 BCD (track 1 INDEX 01 with +150 pregap)
    p = 0x01;
    memset(r, 0xCC, sizeof(r));
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x02, r[2]);
    zexpect_u8_eq(0x00, r[3]);

    // GetTD 00 -> lead-out 60:25 BCD
    p = 0x00;
    memset(r, 0xCC, sizeof(r));
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x60, r[1]);
    zexpect_u8_eq(0x25, r[2]);
    zexpect_u8_eq(0x00, r[3]);

    // Sync(1) now mirrors the last GetTD response.
    memset(sr, 0xCC, sizeof(sr));
    zexpect_s32_eq(CdlComplete, CdSync(1, sr));
    zexpect_u8_eq(CdlStatStandby, sr[0]);
    zexpect_u8_eq(0x60, sr[1]);
    zexpect_u8_eq(0x25, sr[2]);
    zexpect_u8_eq(0x00, r[3]);
}

ZTEST(libcd, data_plus_one_audio) {
    u_char r[8];
    u_char p;
    // 1 data file (75 sectors) + 1 audio file (75 sectors), each in its own
    // FILE entry: this matches how real PSX dumps lay out data + CDDA.
    // T1 abs_sector=0 (+150 -> 0:02), T2 abs_sector=75 (+150 -> 0:03),
    // lead-out=150 (+150 -> 0:04).
    load_cue_multi(1, 75, 75);

    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x01, r[1]);
    zexpect_u8_eq(0x02, r[2]);

    p = 0x01;
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x02, r[2]);

    p = 0x02;
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x03, r[2]);

    p = 0x00;
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x04, r[2]);
}

ZTEST(libcd, multi_audio_six_tracks) {
    u_char r[8];
    // 1 data file (150 sec) + 5 audio files (75 sec each, separate FILE).
    // abs_sector layout: T1=0, T2=150, T3=225, T4=300, T5=375, T6=450,
    // lead-out=525.
    // +150 pregap then /75 -> seconds: T1=2, T2=4, T3=5, T4=6, T5=7, T6=8,
    // lead-out=9.
    load_cue_multi(5, 150, 75);

    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    zexpect_u8_eq(0x01, r[1]);
    zexpect_u8_eq(0x06, r[2]);

    check_td(0x01, 0x00, 0x02);
    check_td(0x03, 0x00, 0x05);
    check_td(0x06, 0x00, 0x08);
    check_td(0x00, 0x00, 0x09); // lead-out
}

ZTEST(libcd, ten_tracks_bcd_boundary) {
    u_char r[8];
    u_char p;
    // 10 tracks in one file forces BCD encoding 0x10.
    load_cue_single(10, 75 * 11, false);

    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    zexpect_u8_eq(0x01, r[1]);
    zprintf("last track must be BCD-encoded\n");
    zexpect_u8_eq(0x10, r[2]);

    // Track 10: INDEX 01 in a single-file CUE places every track at file
    // offset 0, so all tracks share abs_sector 0 -> all report 00:02.
    // (CUE format detail; what we want to assert here is that a BCD-input
    // 0x10 track is accepted and not treated as track 16.)
    p = 0x10;
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &p, r));
    zexpect_u8_eq(0x00, r[1]);
    zexpect_u8_eq(0x02, r[2]);
}

ZTEST(libcd, no_disk_loaded_returns_error_response) {
    u_char r[8];
    u_char p;
    // Don't call load(); explicitly detach disc.
    Psyz_CdSetDiskPath(NULL);

    // Real-hardware "lid closed, no disk" trace (after settling):
    //   STAT 02 STANDBY (drive is empty but spindle eventually settles)
    //   GETTN 01 80 00 00 ret=NoIntr
    // Our model has no spin-up timeline: with no disc we treat the bay as
    // empty / lid-open-equivalent until a disc is provided. Assert error
    // shape rather than exact stat byte.
    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlGetTN, NULL, r));
    zprintf("stat byte must carry CmdErr bit\n");
    zexpect_s32_ne(0, r[0] & CdlStatError);
    zprintf("result[1] must carry error-response sentinel\n");
    zexpect_s32_ne(0, r[1] & 0x80);

    p = 0x01;
    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlGetTD, &p, r));
    zexpect_s32_ne(0, r[0] & CdlStatError);
    zexpect_s32_ne(0, r[1] & 0x80);
}

ZTEST(libcd, invalid_track_number) {
    u_char p = 0x99; // BCD 99, far above any real track
    u_char r[8];
    load_cue_single(1, 100, true);

    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlGetTD, &p, r));
    zexpect_s32_ne(0, r[0] & CdlStatError);
    zexpect_s32_ne(0, r[1] & 0x80);
}

ZTEST(libcd, shell_open_status) {
    u_char r[8];
    u_char p;
    load_cue_single(1, 100, true);
    Psyz_CdShellOpen(1);

    // Nop reports current stat with shell-open bit, no error sentinel.
    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlNop, NULL, r));
    zexpect_s32_ne(0, r[0] & CdlStatShellOpen);

    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlGetTN, NULL, r));
    zprintf("stat byte must carry shell-open bit while lid is open\n");
    zexpect_s32_ne(0, r[0] & CdlStatShellOpen);
    zexpect_s32_ne(0, r[0] & CdlStatError);
    zprintf("error-response sentinel\n");
    zexpect_s32_ne(0, r[1] & 0x80);

    p = 0x01;
    memset(r, 0xCC, sizeof(r));
    zexpect_s32_eq(CdlNoIntr, CdControlB(CdlGetTD, &p, r));
    zexpect_s32_ne(0, r[0] & CdlStatShellOpen);
    zexpect_s32_ne(0, r[0] & CdlStatError);
    zexpect_s32_ne(0, r[1] & 0x80);
}

ZTEST(libcd, shell_cycle_open_then_close) {
    u_char r[8];
    load_cue_single(1, 100, true);

    // Mirrors the full real-hardware lid-cycle:

    // 1. Steady state with disc: STAT 02 STANDBY, all reads succeed.
    zassert_s32_eq(CdlDataReady, CdControlB(CdlNop, NULL, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);

    // 2. Open lid: stat acquires shell-open bit, all reads fail.
    Psyz_CdShellOpen(1);
    zassert_s32_eq(CdlNoIntr, CdControlB(CdlGetTN, NULL, r));
    zexpect_s32_ne(0, r[0] & CdlStatShellOpen);

    // (3) Close lid: latch is sticky until acknowledged by a successful
    // command. The first CdlNop after re-close clears the latch and
    // re-asserts standby.
    Psyz_CdShellOpen(0);
    zassert_s32_eq(CdlDataReady, CdControlB(CdlNop, NULL, r));
    zprintf("first successful command after re-close must clear shell-open\n");
    zexpect_s32_eq(0, r[0] & CdlStatShellOpen);
    zprintf("standby must be re-asserted after re-close\n");
    zexpect_s32_ne(0, r[0] & CdlStatStandby);

    // Subsequent reads succeed normally.
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    zexpect_u8_eq(CdlStatStandby, r[0]);
    zexpect_u8_eq(0x01, r[1]);
    zexpect_u8_eq(0x01, r[2]);
}

// Sync(1) is a non-blocking peek. After a successful command it returns
// CdlComplete plus a copy of the cached last result. After a failing
// command the cached result carries the error-response sentinel.
ZTEST(libcd, sync_peek_caches_last_response) {
    u_char r[8];
    u_char sr[8];
    u_char p;
    load_cue_single(1, 271796, true);

    // Issue GetTN, then peek.
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTN, NULL, r));
    memset(sr, 0xCC, sizeof(sr));
    zexpect_s32_eq(CdlComplete, CdSync(1, sr));
    zexpect_u8_eq(r[0], sr[0]);
    zexpect_u8_eq(r[1], sr[1]); // BCD first track
    zexpect_u8_eq(r[2], sr[2]); // BCD last track

    // Issue a failing GetTD with an invalid track; peek must reflect the
    // error response.
    p = 0x99;
    zassert_s32_eq(CdlNoIntr, CdControlB(CdlGetTD, &p, r));
    memset(sr, 0xCC, sizeof(sr));
    zexpect_s32_eq(CdlDiskError, CdSync(1, sr));
    zexpect_s32_ne(0, sr[1] & 0x80);
}

ZTEST(libcd, responses_stay_inside_the_caller_result_buffer) {
    // CdlGetTD produces four bytes. A caller that sizes its buffer for that
    // response must not have the bytes behind it disturbed.
    struct {
        u_char result[4];
        u_char guard[4];
    } buf;
    u_char track = itob(1);
    size_t i;
    load_cue_single(1, 271796, true);

    memset(&buf, 0xAA, sizeof(buf));
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, &track, buf.result));

    // Repaint the guard so that anything the cached response carries past the
    // four response bytes becomes visible instead of writing 0xAA over 0xAA.
    memset(buf.guard, 0x55, sizeof(buf.guard));
    zexpect_s32_eq(CdlComplete, CdSync(1, buf.result));
    for (i = 0; i < sizeof(buf.guard); i++) {
        if (buf.guard[i] != 0x55) {
            zprintf("CdSync overran result at +%zu\n", i);
        }
        zexpect_u8_eq(0x55, buf.guard[i]);
    }
}

ZTEST(libcd, data_read_returns_header_then_payload) {
    unsigned char expected[SECTOR_SIZE];
    u_char r[8];
    u_char header[12];
    u_char payload[2048];
    u_char mode = CdlModeSize1;
    load_cue_data(4);
    build_data_sector(expected, 0);
    CdControlB(CdlSetmode, &mode, NULL);

    memset(r, 0xCC, sizeof(r));
    zprintf("idle before any read command\n");
    zexpect_s32_eq(CdlNoIntr, CdReady(1, r));

    start_read_at(0);
    memset(r, 0xCC, sizeof(r));
    zassert_s32_eq(CdlDataReady, CdReady(1, r));
    zprintf("stat byte must carry the read bit\n");
    zexpect_s32_ne(0, r[0] & CdlStatRead);

    memset(header, 0xCC, sizeof(header));
    zassert_s32_ne(0, CdGetSector(header, sizeof(header) / 4));
    zprintf("first read must expose the header/subheader at offset 12\n");
    zexpect_u8array_eq(expected + 12, header, sizeof(header));

    memset(payload, 0xCC, sizeof(payload));
    zassert_s32_ne(0, CdGetSector(payload, sizeof(payload) / 4));
    zprintf("second read must expose the user payload at offset 24\n");
    zexpect_u8array_eq(expected + 24, payload, sizeof(payload));

    zprintf("a consumed sector must not be handed out again\n");
    zexpect_s32_eq(0, CdGetSector(payload, sizeof(payload) / 4));

    CdControlB(CdlStop, NULL, NULL);
}

ZTEST(libcd, data_read_advances_through_sectors) {
    u_char mode = CdlModeSize1;
    int sector;
    load_cue_data(4);
    CdControlB(CdlSetmode, &mode, NULL);
    start_read_at(1);

    for (sector = 1; sector < 4; ++sector) {
        unsigned char expected[SECTOR_SIZE];
        u_char header[12];
        u_char payload[2048];
        build_data_sector(expected, sector);

        zprintf("sector %d\n", sector);
        zassert_s32_eq(CdlDataReady, CdReady(1, NULL));

        zassert_s32_ne(0, CdGetSector(header, sizeof(header) / 4));
        zprintf("header of sector %d\n", sector);
        zexpect_u8array_eq(expected + 12, header, sizeof(header));

        zassert_s32_ne(0, CdGetSector(payload, sizeof(payload) / 4));
        zprintf("payload of sector %d\n", sector);
        zexpect_u8array_eq(expected + 24, payload, sizeof(payload));
    }

    CdControlB(CdlStop, NULL, NULL);
}

ZTEST(libcd, data_read_uses_the_read_callback) {
    unsigned char expected[SECTOR_SIZE];
    u_char header[12];
    u_char payload[2048];
    u_char mode = CdlModeSize1;
    int i;
    load_cue_data(4);
    CdControlB(CdlSetmode, &mode, NULL);
    g_read_cb_calls = 0;
    g_read_cb_fails = 0;
    Psyz_CdSetReadCB(fake_read_cb);

    start_read_at(2);
    zassert_s32_eq(CdlDataReady, CdReady(1, NULL));
    zexpect_s32_eq(1, g_read_cb_calls);
    zexpect_u32_eq(2u, g_read_cb_last_sector);
    zexpect_u32_eq(SECTOR_SIZE, g_read_cb_last_size);

    for (i = 0; i < SECTOR_SIZE; ++i) {
        expected[i] = (unsigned char)(0xC0 + i + 2);
    }

    zassert_s32_ne(0, CdGetSector(header, sizeof(header) / 4));
    zexpect_u8array_eq(expected + 12, header, sizeof(header));

    zassert_s32_ne(0, CdGetSector(payload, sizeof(payload) / 4));
    zexpect_u8array_eq(expected + 24, payload, sizeof(payload));

    g_read_cb_fails = 1;
    start_read_at(0);
    zexpect_s32_eq(CdlDiskError, CdReady(1, NULL));

    CdControlB(CdlStop, NULL, NULL);
    Psyz_CdSetReadCB(NULL);
}

ZTEST(libcd, getlocp_reports_the_current_data_sector) {
    u_char r[8];
    u_char header[12];
    u_char payload[2048];
    u_char mode = CdlModeSize1;
    // track 2 starts at sector 80; sector 1000 is 920 into it
    load_cue_multi(1, 80, 1200);
    CdControlB(CdlSetmode, &mode, NULL);
    start_read_at(1000);
    zassert_s32_eq(CdlDataReady, CdReady(1, NULL));

    memset(r, 0xCC, sizeof(r));
    zassert_s32_ne(0, CdControl(CdlGetlocP, NULL, r));
    zprintf("track, index, relative m:s:f, absolute m:s:f in BCD\n");
    zexpect_u8_eq(0x02, r[0]);
    zexpect_u8_eq(0x01, r[1]);
    zexpect_u8_eq(0x00, r[2]);
    zexpect_u8_eq(0x12, r[3]);
    zexpect_u8_eq(0x20, r[4]);
    zexpect_u8_eq(0x00, r[5]);
    zexpect_u8_eq(0x15, r[6]);
    zexpect_u8_eq(0x25, r[7]);

    zassert_s32_ne(0, CdGetSector(header, sizeof(header) / 4));
    zassert_s32_ne(0, CdGetSector(payload, sizeof(payload) / 4));
    memset(r, 0xCC, sizeof(r));
    zassert_s32_ne(0, CdControl(CdlGetlocP, NULL, r));
    zprintf("the position follows the sectors the drive hands out\n");
    zexpect_u8_eq(0x21, r[4]);
    zexpect_u8_eq(0x26, r[7]);

    CdControlB(CdlStop, NULL, NULL);
}

ZTEST(libcd, readn_in_rt_mode_leaves_the_data_path_idle) {
    u_char mode = CdlModeRT;
    load_cue_data(4);

    CdControlB(CdlSetmode, &mode, NULL);
    start_read_at(0);

    zexpect_s32_eq(CdlNoIntr, CdReady(1, NULL));

    CdControlB(CdlStop, NULL, NULL);
    mode = 0;
    CdControlB(CdlSetmode, &mode, NULL);
}

static void mount_bin_cue_pair(
    const unsigned short* data, size_t count, const char* track_type) {
    char cdda_path[PATH_LEN];
    char body[256];
    FILE* f;
    snprintf(cdda_path, sizeof(cdda_path), "%s/cdda.bin", dir);
    f = fopen(cdda_path, "wb");
    if (f == NULL) {
        zprintf("fopen %s\n", cdda_path);
    }
    zassert_ptr_ne(NULL, f);
    fwrite(data, sizeof(unsigned short), count, f);
    fclose(f);
    add_bin(cdda_path);

    snprintf(cue, sizeof(cue), "%s/cdda.cue", dir);
    snprintf(body, sizeof(body),
             "FILE \"cdda.bin\" BINARY\n"
             "  TRACK 01 %s\n"
             "    INDEX 01 00:00:00\n",
             track_type);
    write_text(cue, body);
    mount_cue();
}

// Build a deterministic raw 2352-byte MODE2/2352 XA Form2 sector.
// All ADPCM blocks use shift_in=0 (so shift=12) and filter=0, and every
// nibble is 0x1, so each decoded sample = (1 << 12) = 0x1000 (DC). After
// CD volume (0x3FFF) and main volume (0x3FFF) this comes out as 0x07FE.
static void build_xa_sector_file(
    unsigned char* sector, int sector_idx, unsigned char file) {
    // subheader: channel=0, submode=0x64 (audio|RT|Form2), CI=0x01
    // (stereo, 37800Hz, 4-bit ADPCM)
    const unsigned char sh[4] = {file, 0x00, 0x64, 0x01};
    int i, b;
    memset(sector, 0, 2352);
    // sync
    sector[0] = 0x00;
    for (i = 1; i < 11; ++i)
        sector[i] = 0xFF;
    sector[11] = 0x00;
    // header (BCD MSF + mode 0x02). mm=0, ss=2, ff=sector_idx.
    sector[0x0C] = 0x00;
    sector[0x0D] = 0x02;
    sector[0x0E] = (unsigned char)sector_idx;
    sector[0x0F] = 0x02;
    memcpy(&sector[0x10], sh, 4);
    memcpy(&sector[0x14], sh, 4);
    // 18 ADPCM blocks at offset 0x18
    for (b = 0; b < 18; ++b) {
        unsigned char* blk = &sector[0x18 + b * 128];
        // bytes 0..3 are a copy of bytes 4..7 (sub-block headers)
        // header byte = (filter << 4) | shift_in = 0
        for (i = 0; i < 8; ++i)
            blk[i] = 0x00;
        // bytes 8..15 zero-padded (8-bit extended hdr unused for 4-bit)
        // bytes 16..127 = 28 words x 4 bytes of nibble-packed data.
        // every nibble = 0x1 -> byte = 0x11
        for (i = 16; i < 128; ++i)
            blk[i] = 0x11;
    }
    // remaining bytes (0x18+18*128 .. 0x92F) stay zero (padding + EDC)
}

static void build_xa_sector(unsigned char* sector, int sector_idx) {
    build_xa_sector_file(sector, sector_idx, 0x01);
}

static int make_dir(const char* path) {
#ifndef _WIN32
    return mkdir(path, 0755);
#else
    return _mkdir(path);
#endif
}

ZTEST_SETUP(libcd_playback) {
    int ret;
    reset_state("psyz_libcdplayback_test_");
    ret = make_dir(dir);
    if (ret == -1) {
        zprintf("mkdir failed\n");
        zassert_s32_eq(EEXIST, errno);
    }
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
}

ZTEST_TEARDOWN(libcd_playback) {
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
    Psyz_AudioDestroy();
    cleanup_files();
}

ZTEST(libcd_playback, cdda_playback) {
    // 2 channels * 2 seconds * 44100 frames/s = 176400 stereo frames
    const int channels = 2;
    const int frame_count = channels * PSYZ_SPU_SAMPLE_RATE;
    unsigned short* sample;
    CdlATV fake_vol = {11, 22, 33, 44};
    u_char param[8];
    u_char td_param[4] = {itob(1), 0, 0, 0};
    u_char td_result[4] = {0};
    CdlLOC loc;
    s16* out;
    int i;
    sample = test_calloc(frame_count, sizeof(unsigned short));
    for (i = 0; i < frame_count; i++) {
        sample[i] = 0x6000u;
    }
    mount_bin_cue_pair(sample, frame_count, "AUDIO");

    // Set arbitrary CD volume that will get reset with CdReset(1) anyway
    CdMix(&fake_vol);

    // Initialize CD and audio systems
    CdReset(1);

    // Set CD-DA mode (required for CDDA playback)
    param[0] = CdlModeDA;
    CdControl(CdlSetmode, param, NULL);

    // Get start sector of track 1 via CdlGetTD
    zassert_s32_eq(CdlDataReady, CdControlB(CdlGetTD, td_param, td_result));

    // Build CdlLOC from BCD MM:SS returned by CdlGetTD (sector = 0)
    loc.minute = td_result[1];
    loc.second = td_result[2];
    loc.sector = 0x00;
    loc.track = 0x00;
    CdControlB(CdlSeekP, (u_char*)&loc, NULL);

    // Stop the SDL audio thread from pulling samples and take the lock so we
    // drive Psyz_SpuPullSamples deterministically and avoid race conditions.
    Psyz_AudioPause();
    Psyz_AudioLock();

    // Start actual CDDA playback. CdlPlay does not touch the SDL pause state,
    // so the stream stays paused until the test releases.
    CdControlB(CdlPlay, NULL, NULL);

    // Pull samples from the SPU that would've been otherwise read from the HW
    out = test_calloc(frame_count * 2, sizeof(s16));
    Psyz_SpuPullSamples(out, frame_count);

    // Reverse Psyz_AudioLock
    Psyz_AudioUnlock();

    for (i = 0; i < frame_count / 2; i++) {
        if (out[i * 2 + 0] != 0x2FFE) {
            zprintf("frame %d left channel unexpected value\n", i);
        }
        zassert_s16_eq(0x2FFE, out[i * 2 + 0]);
        if (out[i * 2 + 1] != 0x2FFE) {
            zprintf("frame %d right channel unexpected value\n", i);
        }
        zassert_s16_eq(0x2FFE, out[i * 2 + 1]);
    }
}

ZTEST(libcd_playback, xa_playback) {
    // Each MODE2/2352 XA Form2 sector decodes to 18*4*28 = 2016 stereo frames
    // at 37800 Hz, which yields 2016 * 44100/37800 = 2352 output frames at
    // 44100 Hz. 16 sectors -> ~37632 output frames.
    const int kSectors = 16;
    const int frame_count = 37632;
    // Skip the first 64 frames where the Hermite ring is filling and the
    // resampler is settling toward the DC plateau.
    const int kSettle = 64;
    unsigned short* sample;
    unsigned char* raw;
    u_char param[8];
    CdlLOC loc;
    short* out;
    int i;
    sample = test_calloc(kSectors * 2352 / 2, sizeof(unsigned short));
    raw = (unsigned char*)sample;
    for (i = 0; i < kSectors; i++) {
        build_xa_sector(raw + i * 2352, i);
    }
    mount_bin_cue_pair(sample, kSectors * 2352 / 2, "MODE2/2352");

    // Initialize CD and audio systems
    CdReset(1);

    // Set XA-ADPCM mode at double-speed with channel filtering enabled.
    param[0] = CdlModeSpeed | CdlModeRT | CdlModeSF;
    CdControlB(CdlSetmode, param, NULL);

    // Filter to file=1 channel=0, matching the synthesized subheader.
    param[0] = 1;
    param[1] = 0;
    CdControlB(CdlSetfilter, param, NULL);

    CdIntToPos(0, &loc);
    CdControl(CdlSetloc, (u_char*)&loc, NULL);

    // Stop the SDL audio thread from pulling samples and take the lock so we
    // drive Psyz_SpuPullSamples deterministically and avoid race conditions.
    Psyz_AudioPause();
    Psyz_AudioLock();

    // Kick off XA-ADPCM streaming.
    CdControl(CdlReadN, NULL, NULL);

    out = test_calloc(frame_count * 2, sizeof(short));
    Psyz_SpuPullSamples(out, frame_count);

    // Reverse Psyz_AudioLock
    Psyz_AudioUnlock();

    // Each XA sample decodes to 0x1000 -> after cd_vol (0x3FFF >> 15) and
    // main_vol (0x3FFF >> 14) the steady-state output is 0x07FE per channel.
    for (i = kSettle; i < frame_count; ++i) {
        if (out[i * 2 + 0] != 0x07FE) {
            zprintf("frame %d left channel unexpected value\n", i);
        }
        zassert_s16_eq(0x07FE, out[i * 2 + 0]);
        if (out[i * 2 + 1] != 0x07FE) {
            zprintf("frame %d right channel unexpected value\n", i);
        }
        zassert_s16_eq(0x07FE, out[i * 2 + 1]);
    }
}

ZTEST(libcd_playback, data_read_interrupts_xa_playback) {
    const int kSectors = 16;
    const int kFrames = 512;
    unsigned short* sample;
    unsigned char* raw;
    u_char param[8];
    CdlLOC loc;
    short* out;
    short* drain;
    bool streaming = false;
    int i;
    sample = test_calloc(kSectors * 2352 / 2, sizeof(unsigned short));
    raw = (unsigned char*)sample;
    for (i = 0; i < kSectors; i++) {
        build_xa_sector(raw + i * 2352, i);
    }
    mount_bin_cue_pair(sample, kSectors * 2352 / 2, "MODE2/2352");

    CdReset(1);

    param[0] = CdlModeSpeed | CdlModeRT | CdlModeSF;
    CdControlB(CdlSetmode, param, NULL);
    param[0] = 1;
    param[1] = 0;
    CdControlB(CdlSetfilter, param, NULL);

    CdIntToPos(0, &loc);
    CdControl(CdlSetloc, (u_char*)&loc, NULL);

    Psyz_AudioPause();
    Psyz_AudioLock();

    out = test_calloc(kFrames * 2, sizeof(short));
    CdControl(CdlReadN, NULL, NULL);
    Psyz_SpuPullSamples(out, kFrames);

    for (i = 0; i < kFrames * 2; i++) {
        if (out[i] != 0) {
            streaming = true;
            break;
        }
    }
    zprintf("XA stream produced no audio to interrupt\n");
    zassert_s32_ne(0, streaming);

    param[0] = CdlModeSpeed;
    CdControlB(CdlSetmode, param, NULL);
    CdControl(CdlReadN, NULL, NULL);
    zassert_s32_eq(CdlDataReady, CdReady(1, NULL));

    drain = test_calloc(4096 * 2, sizeof(short));
    Psyz_SpuPullSamples(drain, 4096);

    for (i = 0; i < kFrames * 2; i++) {
        out[i] = (short)0x1234;
    }
    Psyz_SpuPullSamples(out, kFrames);
    Psyz_AudioUnlock();

    for (i = 0; i < kFrames * 2; ++i) {
        if (out[i] != 0) {
            zprintf("XA kept streaming after a data read, sample %d\n", i);
        }
        zassert_s16_eq(0, out[i]);
    }
}

ZTEST(libcd_playback, xa_filtered_stream_ends_in_silence) {
    // Mirrors back-to-back voice clips: a short clip in file 1, the next clip
    // in file 2, then unrelated audio that happens to reuse file 1 further
    // into the disc. Once file 1 runs out the filter rejects every sector, so
    // hardware goes silent instead of jumping to the later file 1 sectors.
    const int kClip = 8;
    const int kOther = 128;
    const int kLater = 8;
    const int kSectors = kClip + kOther + kLater;
    const int kClipFrames = kClip * 2352; // output frames at 44100 Hz
    const int frame_count = kClipFrames * 3;
    const int kSettle = 64;
    unsigned short* sample;
    unsigned char* raw;
    u_char param[8];
    CdlLOC loc;
    short* out;
    int i;
    sample = test_calloc(kSectors * 2352 / 2, sizeof(unsigned short));
    raw = (unsigned char*)sample;
    for (i = 0; i < kSectors; i++) {
        unsigned char file = (i < kClip || i >= kClip + kOther) ? 1 : 2;
        build_xa_sector_file(raw + i * 2352, i, file);
    }
    mount_bin_cue_pair(sample, kSectors * 2352 / 2, "MODE2/2352");

    CdReset(1);

    param[0] = CdlModeSpeed | CdlModeRT | CdlModeSF;
    CdControlB(CdlSetmode, param, NULL);
    param[0] = 1;
    param[1] = 0;
    CdControlB(CdlSetfilter, param, NULL);

    CdIntToPos(0, &loc);
    CdControl(CdlSetloc, (u_char*)&loc, NULL);

    Psyz_AudioPause();
    Psyz_AudioLock();

    CdControl(CdlReadN, NULL, NULL);

    out = test_calloc(frame_count * 2, sizeof(short));
    Psyz_SpuPullSamples(out, frame_count);

    Psyz_AudioUnlock();

    for (i = kSettle; i < kClipFrames - kSettle; ++i) {
        if (out[i * 2 + 0] != 0x07FE || out[i * 2 + 1] != 0x07FE) {
            zprintf("clip frame %d unexpected value\n", i);
        }
        zassert_s16_eq(0x07FE, out[i * 2 + 0]);
        zassert_s16_eq(0x07FE, out[i * 2 + 1]);
    }
    for (i = kClipFrames + kSettle; i < frame_count; ++i) {
        if (out[i * 2 + 0] != 0 || out[i * 2 + 1] != 0) {
            zprintf("frame %d played audio past the end of the clip\n", i);
        }
        zassert_s16_eq(0, out[i * 2 + 0]);
        zassert_s16_eq(0, out[i * 2 + 1]);
    }
}

static void load_iso_table(const char* name, int lba) {
    CdlFILE file;
    u_char result[8];
    u_char buf[2 * 2048 + 64];
    unsigned char expected[SECTOR_SIZE];
    int guard = 1;
    int i;
    zprintf("%s\n", name);
    zassert_ptr_ne(NULL, CdSearchFile(&file, (char*)name));
    zexpect_s32_eq(lba, CdPosToInt(&file.pos));
    zexpect_s32_eq(1, CdControl(CdlSetloc, (u_char*)&file.pos, NULL));
    memset(buf, 0xCC, sizeof(buf));
    zexpect_s32_eq(1, CdRead(2, (u_long*)buf, CdlModeSpeed));
    zexpect_s32_eq(0, CdReadSync(0, result));
    zexpect_s32_eq(CdlModeSpeed, CdMode());
    for (i = 0; i < 2; i++) {
        build_iso_sector(expected, lba + i);
        zexpect_u8array_eq(expected + 24, buf + i * 2048, 2048);
    }
    for (i = 2 * 2048; i < (int)sizeof(buf); i++) {
        guard &= buf[i] == 0xCC;
    }
    zprintf("bytes after the last sector must stay untouched\n");
    zexpect_s32_eq(1, guard);
    zexpect_s32_eq(1, CdControl(CdlPause, NULL, NULL));
}

ZTEST(libcd, iso_reads_land_on_their_sectors) {
    static const char* names[] = {
        "\\STR\\INTRO01.STR;1",
        "\\STR\\OPEN.STR;1",
        "\\XA\\BGM.XA;1",
        "\\XA\\DEMO.XA;1",
    };
    static const int expected[] = {
        16, 18, 22, ISO_DAT, ISO_DAT + 1, 23, 24, 22, ISO_BIN, ISO_BIN + 1,
    };
    u_char mode = CdlModeSpeed | CdlModeSize1;
    CdlFILE file;
    int i;
    load_cue_sectors(ISO_SECTORS, build_iso_sector);
    g_iso_read_count = 0;
    Psyz_CdSetReadCB(log_read_cb);
    zassert_s32_eq(1, CdInit());
    zassert_s32_eq(1, CdControl(CdlSetmode, &mode, NULL));

    load_iso_table("\\GAME_01.DAT;1", ISO_DAT);
    for (i = 0; i < 4; i++) {
        zprintf("%s\n", names[i]);
        zassert_ptr_ne(NULL, CdSearchFile(&file, (char*)names[i]));
        zexpect_s32_eq(31 + i, CdPosToInt(&file.pos));
        zexpect_s32_eq(1, CdControl(CdlPause, NULL, NULL));
    }
    load_iso_table("\\GAME_01.BIN;1", ISO_BIN);

    zprintf("each directory is read once, each file at its own sectors\n");
    zassert_s32_eq(sizeof(expected) / sizeof(*expected), g_iso_read_count);
    for (i = 0; i < g_iso_read_count; i++) {
        zprintf("read %d\n", i);
        zexpect_s32_eq(expected[i], g_iso_reads[i]);
    }
}

static struct {
    int state;
    int lba;
    int rem;
    int calls;
    int bad;
    u_char* dst;
} g_stream;

static int g_ready_count;

static void stream_ready(u_char intr, u_char* result) {
    u_char header[12];
    u_long scratch[0x200];
    int n;
    (void)result;
    g_stream.calls++;
    if (g_stream.state != 1 || intr != CdlDataReady ||
        CdReady(1, NULL) != CdlDataReady) {
        g_stream.bad++;
        return;
    }
    DsGetSector(header, 3);
    if (CdPosToInt((CdlLOC*)header) != g_stream.lba + 1) {
        g_stream.bad++;
        return;
    }
    g_stream.lba++;
    if (g_stream.rem > 0x800) {
        DsGetSector(g_stream.dst, 0x200);
        g_stream.dst += 0x800;
        g_stream.rem -= 0x800;
        return;
    }
    n = (g_stream.rem + 3) >> 2;
    DsGetSector(g_stream.dst, n);
    if (n != 0x200) {
        DsGetSector(scratch, 0x200 - n);
    }
    g_stream.rem = 0;
    CdReadyCallback(NULL);
    CdControl(CdlPause, NULL, NULL);
    g_stream.state = 2;
}

static void count_ready(u_char intr, u_char* result) {
    (void)intr;
    (void)result;
    g_ready_count++;
}

ZTEST(libcd, ready_callback_streams_mode2_sectors) {
    u_char mode = CdlModeSpeed | CdlModeSize1;
    u_char res[8];
    unsigned char expected[SECTOR_SIZE];
    CdlLOC loc;
    u_char* dst;
    int guard = 1;
    int i, count;
    load_cue_sectors(ISO_SECTORS, build_iso_sector);
    zassert_s32_eq(1, CdInit());
    CdControlB(CdlPause, NULL, NULL);
    dst = test_calloc(ISO_BIN_SIZE + 64, 1);
    memset(dst, 0xCC, ISO_BIN_SIZE + 64);
    memset(&g_stream, 0, sizeof(g_stream));
    g_stream.lba = ISO_BIN - 1;
    g_stream.rem = ISO_BIN_SIZE;
    g_stream.dst = dst;

    for (i = 0; i < 100; i++) {
        CdControlB(CdlNop, NULL, res);
        if (!(res[0] & CdlStatSeek)) {
            break;
        }
    }
    zassert_s32_gt(i, 100);
    for (i = 0; i < 100 && CdReady(1, NULL) != CdlNoIntr; i++) {
    }
    zassert_s32_gt(i, 100);
    zassert_s32_eq(1, CdControl(CdlSetmode, &mode, NULL));
    CdIntToPos(ISO_BIN, &loc);
    zassert_s32_eq(1, CdControl(CdlSetloc, (u_char*)&loc, NULL));
    zexpect_ptr_eq(NULL, CdReadyCallback(stream_ready));
    zassert_s32_eq(1, CdControl(CdlReadN, NULL, NULL));
    zprintf("no sector is delivered before the first vertical blank\n");
    zexpect_s32_eq(0, g_stream.calls);
    g_stream.state = 1;
    for (i = 0; i < 60 && g_stream.state != 2; i++) {
        VSync(0);
    }
    zprintf("one call per sector, in sector order\n");
    zexpect_s32_eq(2, g_stream.state);
    zexpect_s32_eq(0, g_stream.bad);
    zexpect_s32_eq(3, g_stream.calls);
    zexpect_s32_eq(ISO_BIN + 2, g_stream.lba);
    for (i = 0; i < 3; i++) {
        int len = i < 2 ? 2048 : ISO_BIN_SIZE - 2 * 2048;
        build_iso_sector(expected, ISO_BIN + i);
        zprintf("sector %d\n", ISO_BIN + i);
        zexpect_u8array_eq(expected + 24, dst + i * 2048, len);
    }
    for (i = ISO_BIN_SIZE; i < ISO_BIN_SIZE + 64; i++) {
        guard &= dst[i] == 0xCC;
    }
    zprintf("bytes after the file must stay untouched\n");
    zexpect_s32_eq(1, guard);

    g_ready_count = 0;
    CdReadyCallback(count_ready);
    zassert_s32_eq(1, CdControl(CdlSetloc, (u_char*)&loc, NULL));
    zassert_s32_eq(1, CdControl(CdlReadN, NULL, NULL));
    VSync(0);
    zassert_s32_ne(0, g_ready_count);
    zassert_s32_eq(1, CdControl(CdlPause, NULL, NULL));
    count = g_ready_count;
    for (i = 0; i < 10; i++) {
        VSync(0);
    }
    zprintf("a paused drive delivers nothing\n");
    zexpect_s32_eq(count, g_ready_count);
}

ZTEST(libcd, seekl_holds_back_the_first_sector) {
    CdlLOC loc;
    u_char mode = CdlModeSpeed;
    u_char r[8];
    int pos;
    load_cue_data(100);
    CdControlB(CdlSetmode, &mode, NULL);
    CdIntToPos(25, &loc);
    CdControlB(CdlSetloc, (u_char*)&loc, NULL);
    CdControlB(CdlSeekL, (u_char*)&loc, NULL);
    g_ready_count = 0;
    CdReadyCallback(count_ready);
    CdControlB(CdlReadS, NULL, NULL);
    VSync(3);

    zassert_s32_ne(0, CdControl(CdlGetlocP, NULL, r));
    pos = (btoi(r[5]) * 60 + btoi(r[6])) * 75 + btoi(r[7]) - 150;
    zprintf("three vblanks after the seek the head is near the target\n");
    zexpect_s32_ge(5, pos);
    zexpect_s32_le(30, pos);

    CdControlB(CdlPause, NULL, NULL);
    mode = 0;
    CdControlB(CdlSetmode, &mode, NULL);
}

static void data_done(void) {}

ZTEST(libcd, library_calls_answer_as_the_bios_does) {
    CdlATV atv = {128, 0, 128, 0};
    CdlFILE file;
    CdlLOC loc;
    u_char res[8];
    void* prev;
    load_cue_sectors(ISO_SECTORS, build_iso_sector);
    zassert_s32_eq(1, CdInit());

    zassert_ptr_ne(NULL, CdSearchFile(&file, "\\XA\\BGM.XA;1"));
    zexpect_s32_eq(33, CdPosToInt(&file.pos));
    CdIntToPos(ISO_DAT, &loc);
    zexpect_s32_eq(1, CdControl(CdlSetloc, (u_char*)&loc, NULL));
    zexpect_s32_eq(ISO_DAT, CdPosToInt(CdLastPos()));

    zexpect_s32_eq(1, CdControlF(CdlGetlocP, NULL));
    zexpect_s32_eq(CdlGetlocP, CdLastCom());
    zexpect_s32_eq(1, CdControlB(CdlPause, NULL, res));
    zexpect_s32_eq(1, CdControlB(CdlNop, NULL, res));
    zexpect_s32_eq(CdlComplete, CdSync(1, res));

    zexpect_s32_eq(CdlNoIntr, CdReady(1, NULL));
    zexpect_s32_eq(1, CdControlF(CdlReadS, NULL));
    zexpect_s32_eq(CdlReadS, CdLastCom());
    zexpect_s32_eq(CdlDataReady, CdReady(1, NULL));
    zexpect_s32_eq(1, CdControl(CdlPause, NULL, NULL));

    zexpect_ptr_eq(NULL, CdReadyCallback(count_ready));
    zexpect_ptr_eq(count_ready, CdReadyCallback(NULL));
    zexpect_s32_eq(1, CdMix(&atv));
    prev = CdDataCallback(data_done);
    zexpect_ptr_eq(data_done, CdDataCallback((void (*)())prev));
    _bu_init();
}

// A synthetic STR movie: 24-bit MDEC frames streamed into the St* ring by
// CdRead2, XA audio sectors interleaved, decoded strip by strip.
enum { STR_W = 320, STR_H = 240, STR_FRAMES = 6, STR_QUANT = 4 };
enum { STR_LBA = 16, STR_SLOTS = 64, STR_RING = 16, STR_CHUNK = 2016 };
enum { STR_MB = (STR_W / 16) * (STR_H / 16), STR_STRIPS = STR_W / 16 };
enum { STR_RUNLEVEL = (STR_MB * 10 + 31) & ~31, STR_MAX = 8 * STR_CHUNK };
enum { STR_OUT = 12 * STR_H, STR_GUARD = 32, STR_LOG = 256 };
// |error| bound per channel, from the MDEC's fixed-point steps
#define STR_TOLERANCE 3.0

enum { STR_PAD, STR_VIDEO, STR_AUDIO };
static struct {
    unsigned char bits[STR_FRAMES][STR_MAX];
    int bytes[STR_FRAMES], sectors[STR_FRAMES];
    unsigned char kind[STR_LBA + STR_SLOTS], frame[STR_LBA + STR_SLOTS],
        chunk[STR_LBA + STR_SLOTS], channel[STR_LBA + STR_SLOTS];
    int audio, video, end;
} str;

static struct {
    int read_lba[STR_LOG], reads, last_lba;
    int delivered_lba[STR_LOG], deliveries;
    int vsyncs, rate_bad, frames, header_bad, data_bad, vlc_bad, wraps;
    int strips[STR_FRAMES], strip_count, guard_bad;
    double max_error;
    CdlCB st_ready;
} g_str;

static u_long str_ring[STR_RING * 2048 / sizeof(u_long)];
static u_long str_vlc[STR_RUNLEVEL + 1];
static u_long str_strip[STR_OUT + STR_GUARD];
static unsigned char str_image[STR_H][STR_W][3];

static void str_put(unsigned char* p, size_t* bit, unsigned value, int count) {
    while (count--) {
        unsigned b = *bit % 16;
        if ((value >> count) & 1) {
            p[*bit / 16 * 2 + (b < 8)] |= 0x80 >> (b % 8);
        }
        ++*bit;
    }
}

// levels: Cr, Cb and four luma DCs per macroblock; two AC terms per luma block
static int str_dc(int f, int mb, int block) {
    if (block == 0) {
        return (mb * 37 + f * 11) % 241 - 120;
    }
    if (block == 1) {
        return (mb * 53 + f * 19 + 60) % 241 - 120;
    }
    return (mb * 13 + block * 41 + f * 17) % 321 - 160;
}

static int str_ac(int f, int mb, int block, int k) {
    int v = (mb * 7 + block * 3 + k * 5 + f) % 16;
    return v < 8 ? v + 1 : 7 - v;
}

static void str_encode(int f) {
    unsigned char* p = str.bits[f];
    size_t bit = 0;
    int mb, block, k;
    memset(p, 0, STR_MAX);
    p[0] = STR_RUNLEVEL & 0xFF;
    p[1] = STR_RUNLEVEL >> 8;
    p[3] = 0x38;
    p[4] = STR_QUANT;
    p[6] = 2;
    for (mb = 0; mb < STR_MB; mb++) {
        for (block = 0; block < 6; block++) {
            str_put(p + 8, &bit, str_dc(f, mb, block) & 0x3FF, 10);
            for (k = 0; block >= 2 && k < 2; k++) {
                str_put(p + 8, &bit, 1, 6);
                str_put(p + 8, &bit, str_ac(f, mb, block, k) & 0x3FF, 16);
            }
            str_put(p + 8, &bit, 2, 2);
        }
    }
    str_put(p + 8, &bit, 0x1FF, 10);
    str.bytes[f] = (int)((8 + (bit + 15) / 16 * 2 + 3) & ~3u);
    str.sectors[f] = (str.bytes[f] + STR_CHUNK - 1) / STR_CHUNK;
}

// every eighth sector is XA audio, the second of them on another channel
static void str_layout(void) {
    int lba, f = 0, chunk = 0, audio = 0;
    memset(&str.kind, 0, sizeof(str.kind));
    str.audio = str.video = 0;
    for (f = 0; f < STR_FRAMES; f++) {
        str_encode(f);
    }
    f = 0;
    for (lba = STR_LBA; f < STR_FRAMES; lba++) {
        if ((lba - STR_LBA) % 8 == 3) {
            str.kind[lba] = STR_AUDIO;
            str.channel[lba] = audio++ == 1 ? 2 : 1;
            str.audio++;
            continue;
        }
        str.kind[lba] = STR_VIDEO;
        str.frame[lba] = (unsigned char)f;
        str.chunk[lba] = (unsigned char)chunk;
        str.video++;
        if (++chunk == str.sectors[f]) {
            chunk = 0;
            f++;
        }
    }
    str.end = lba;
}

static void str_sector(unsigned char* sector, int lba) {
    CdlLOC loc;
    unsigned char* user = sector + 24;
    int f = str.frame[lba], chunk = str.chunk[lba];
    memset(sector, 0, SECTOR_SIZE);
    memset(sector + 1, 0xFF, 10);
    CdIntToPos(lba, &loc);
    sector[12] = loc.minute;
    sector[13] = loc.second;
    sector[14] = loc.sector;
    sector[15] = 2;
    if (str.kind[lba] == STR_AUDIO) {
        sector[16] = sector[20] = 1;
        sector[17] = sector[21] = str.channel[lba];
        sector[18] = sector[22] = 0x64;
        sector[19] = sector[23] = 0x01;
        memset(user, 0x55, 2324);
        return;
    }
    if (str.kind[lba] != STR_VIDEO) {
        return;
    }
    sector[16] = sector[20] = 1;
    sector[17] = sector[21] = 1;
    sector[18] = sector[22] = 0x48;
    user[0] = 0x60;
    user[1] = 0x01;
    user[2] = 0x01;
    user[3] = 0x80;
    user[4] = (unsigned char)chunk;
    user[6] = (unsigned char)str.sectors[f];
    user[8] = (unsigned char)(f + 1);
    put_le32(user + 12, str.bytes[f]);
    user[16] = STR_W & 0xFF;
    user[17] = STR_W >> 8;
    user[18] = STR_H & 0xFF;
    memcpy(user + 32, str.bits[f] + chunk * STR_CHUNK, STR_CHUNK);
}

static int str_read(struct PsyzCdRead* read) {
    if ((int)read->sector >= STR_LBA + STR_SLOTS) {
        return -1;
    }
    if (g_str.reads < STR_LOG) {
        g_str.read_lba[g_str.reads] = (int)read->sector;
    }
    g_str.reads++;
    g_str.last_lba = (int)read->sector;
    str_sector(read->buffer, (int)read->sector);
    return (int)read->size;
}

static void str_ready(u_char intr, u_char* result) {
    if (g_str.deliveries < STR_LOG) {
        g_str.delivered_lba[g_str.deliveries] = g_str.last_lba;
    }
    g_str.deliveries++;
    g_str.st_ready(intr, result);
}

static void str_frame_ready(void) {}

static void str_strip_done(void) {
    int n = g_str.strip_count++;
    int x, y;
    if (n < STR_STRIPS) {
        for (y = 0; y < STR_H; y++) {
            for (x = 0; x < 16; x++) {
                memcpy(str_image[y][n * 16 + x],
                       (unsigned char*)str_strip + (y * 16 + x) * 3, 3);
            }
        }
    }
    if (n + 1 < STR_STRIPS) {
        DecDCTout(str_strip, STR_OUT);
    }
}

static void str_vsync(void) {
    int before = g_str.reads;
    VSync(0);
    // vblanks of the SeekL before the first sector are not counted
    if (g_str.reads == 0) {
        return;
    }
    g_str.vsyncs++;
    if (g_str.reads - before != 2 && g_str.reads - before != 3) {
        g_str.rate_bad++;
    }
}

// the frame as the encoder meant it: IDCT of the dequantised levels, then
// YCbCr to RGB, in double precision
static void str_check_frame(int f) {
    // cos((2x + 1) * pi / 16), and the 1 / sqrt(2) of the zero frequency
    static const double cosine[8] = {
        0.98078528040323,  0.83146961230255,  0.55557023301960,
        0.19509032201613,  -0.19509032201613, -0.55557023301960,
        -0.83146961230255, -0.98078528040323};
    const double ac = 16.0 * STR_QUANT / 8 / 4 * 0.70710678118655;
    int x, y, c;
    for (y = 0; y < STR_H; y++) {
        for (x = 0; x < STR_W; x++) {
            int mb = x / 16 * (STR_H / 16) + y / 16;
            int lx = x % 16, ly = y % 16;
            int block = 2 + (ly >> 3) * 2 + (lx >> 3);
            double cos_x = cosine[lx & 7], cos_y = cosine[ly & 7];
            double luma = str_dc(f, mb, block) * 2 / 8.0 +
                          ac * str_ac(f, mb, block, 0) * cos_x +
                          ac * str_ac(f, mb, block, 1) * cos_y;
            double cr = str_dc(f, mb, 0) * 2 / 8.0;
            double cb = str_dc(f, mb, 1) * 2 / 8.0;
            double rgb[3];
            rgb[0] = 128 + luma + 1.402 * cr;
            rgb[1] = 128 + luma - 0.344136 * cb - 0.714136 * cr;
            rgb[2] = 128 + luma + 1.772 * cb;
            for (c = 0; c < 3; c++) {
                double e = str_image[y][x][c] - rgb[c];
                e = e < 0 ? -e : e;
                if (e > g_str.max_error) {
                    g_str.max_error = e;
                }
            }
        }
    }
}

// a movie player: DecDCTReset(0), StSetRing, StSetStream, CdRead2(0x1C8),
// StGetNext per frame, DecDCTvlc, DecDCTin(buf, 1), DecDCTout by strips
static void str_replay(void) {
    u_char param[4];
    CdlLOC loc;
    u_long *addr, *header;
    int f, i, slot, previous = -1;
    str_layout();
    zassert_s32_ge(3, str.sectors[0]);
    zassert_s32_gt(STR_RING, str.video);
    zassert_s32_ge(str.end, STR_LBA + STR_SLOTS);
    memset(&g_str, 0, sizeof(g_str));
    load_cue_sectors(STR_LBA + STR_SLOTS, str_sector);
    Psyz_CdSetReadCB(str_read);
    zassert_s32_eq(1, CdInit());

    DecDCTReset(0);
    DecDCToutCallback(str_strip_done);
    StSetRing(str_ring, STR_RING);
    StSetStream(1, 1, 0x0FFFFFFF, str_frame_ready, 0);
    param[0] = 1;
    param[1] = 1;
    CdControl(CdlSetfilter, param, NULL);
    CdIntToPos(STR_LBA, &loc);
    CdControlB(CdlSeekL, (u_char*)&loc, NULL);
    zassert_s32_eq(1, CdRead2(0x1C8));
    g_str.st_ready = CdReadyCallback(str_ready);
    zassert_ptr_ne(NULL, g_str.st_ready);

    for (f = 0; f < STR_FRAMES; f++) {
        StHEADER* h;
        for (i = 0; StGetNext(&addr, &header); i++) {
            if (i == 600) {
                zprintf("frame %d never arrived\n", f + 1);
                goto stop;
            }
            str_vsync();
        }
        h = (StHEADER*)header;
        if (h->frameCount != (u32)(f + 1) || h->nSectors != str.sectors[f] ||
            h->secCount != 0 || h->width != STR_W || h->height != STR_H) {
            g_str.header_bad++;
        }
        if (memcmp(addr, str.bits[f], str.bytes[f])) {
            g_str.data_bad++;
        }
        slot = (int)(((unsigned char*)addr - (unsigned char*)str_ring -
                      STR_RING * sizeof(StHEADER)) /
                     STR_CHUNK);
        if (slot < previous) {
            g_str.wraps++;
        }
        previous = slot;
        if (DecDCTvlc(addr, str_vlc)) {
            g_str.vlc_bad++;
        }
        StFreeRing(addr);
        g_str.frames++;

        DecDCTReset(1);
        memset(str_image, 0, sizeof(str_image));
        for (i = 0; i < STR_GUARD; i++) {
            str_strip[STR_OUT + i] = 0xA5A5A5A5;
        }
        g_str.strip_count = 0;
        DecDCTin(str_vlc, 1);
        DecDCTout(str_strip, STR_OUT);
        for (i = 0; i < 10 && g_str.strip_count < STR_STRIPS; i++) {
            str_vsync();
        }
        str_vsync();
        g_str.strips[f] = g_str.strip_count;
        for (i = 0; i < STR_GUARD; i++) {
            g_str.guard_bad += str_strip[STR_OUT + i] != 0xA5A5A5A5;
        }
        str_check_frame(f);
    }
stop:
    for (i = 0; i < 10 && !CdControlB(CdlPause, NULL, NULL); i++) {
    }
    param[0] = CdlModeSpeed;
    CdControl(CdlSetmode, param, NULL);
    StUnSetRing();
    DecDCToutCallback(NULL);
}

ZTEST_SETUP(stream) {
    int ret;
    reset_state("psyz_stream_test_");
    ret = mkdir(dir, 0755);
    zassert_s32_eq(0, ret);
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
}

ZTEST_TEARDOWN(stream) {
    StUnSetRing();
    DecDCToutCallback(NULL);
    Psyz_CdSetReadCB(NULL);
    Psyz_CdSetDiskPath(NULL);
    Psyz_CdShellOpen(0);
    Psyz_AudioDestroy();
    cleanup_files();
}

ZTEST(stream, ring_sectors_arrive_in_lba_order) {
    int i, bad = 0;
    str_replay();
    zassert_s32_eq(STR_FRAMES, g_str.frames);
    zassert_s32_gt(g_str.reads, STR_LOG);
    for (i = 0; i < g_str.reads; i++) {
        bad += g_str.read_lba[i] != STR_LBA + i;
    }
    zprintf("one sector after the other from the SeekL position\n");
    zexpect_s32_eq(0, bad);
    zprintf("150 sectors a second: 2 or 3 per vertical blank\n");
    zexpect_s32_eq(0, g_str.rate_bad);
    zexpect_s32_eq(g_str.vsyncs * 150 / 60, g_str.reads);
}

ZTEST(stream, get_next_gives_whole_frames) {
    str_replay();
    zassert_s32_eq(STR_FRAMES, g_str.frames);
    zprintf("StHEADER frame number, sector count, width and height\n");
    zexpect_s32_eq(0, g_str.header_bad);
    zprintf("frame data contiguous in the ring\n");
    zexpect_s32_eq(0, g_str.data_bad);
    zprintf("the ring wrapped\n");
    zexpect_s32_ne(0, g_str.wraps);
}

ZTEST(stream, out_callback_fires_once_per_strip) {
    int f;
    str_replay();
    zassert_s32_eq(STR_FRAMES, g_str.frames);
    for (f = 0; f < STR_FRAMES; f++) {
        zprintf("frame %d\n", f + 1);
        zexpect_s32_eq(STR_STRIPS, g_str.strips[f]);
    }
    zprintf("each strip is 12 * h words\n");
    zexpect_s32_eq(0, g_str.guard_bad);
}

ZTEST(stream, rgb24_frames_match_the_source) {
    str_replay();
    zassert_s32_eq(STR_FRAMES, g_str.frames);
    zexpect_s32_eq(0, g_str.vlc_bad);
    zprintf("largest channel error %.3f\n", g_str.max_error);
    zexpect_s32_eq(1, g_str.max_error <= STR_TOLERANCE);
}

ZTEST(stream, audio_sectors_bypass_the_ring) {
    int i, audio_read = 0, audio_delivered = 0;
    str_replay();
    zassert_s32_eq(STR_FRAMES, g_str.frames);
    for (i = 0; i < g_str.reads && i < STR_LOG; i++) {
        audio_read += str.kind[g_str.read_lba[i]] == STR_AUDIO;
    }
    for (i = 0; i < g_str.deliveries && i < STR_LOG; i++) {
        audio_delivered += str.kind[g_str.delivered_lba[i]] == STR_AUDIO;
    }
    zprintf("every audio sector of the movie was read\n");
    zexpect_s32_eq(str.audio, audio_read);
    zprintf("none was handed to the stream\n");
    zexpect_s32_eq(0, audio_delivered);
    zexpect_s32_eq(g_str.reads - audio_read, g_str.deliveries);
}
