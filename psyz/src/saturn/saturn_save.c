/* saturn_save.c -- PS1 card files on the Saturn internal backup device
 * One BIOS record per
 * PS1 file, key = 11-char BIOS filename derived from the PS1 name; the record
 * data is a 36-byte big-endian header (magic PSYZ, version 1, header size 36,
 * PS1 size, PS1 name[24]) then the PS1 data. Only records with that header are
 * ours; anything else is foreign and never read past its directory entry,
 * written, deleted or listed. BUP_Format and BUP_SelPart are never called.
 * Reset guard (saturn_pad RESDISA/RESENAB) around BUP_Init, BUP_Write and
 * BUP_Delete only (Backup Library guide, mirror p01_10). Idle until the first
 * bu00: call. Types, status values, write modes and the stub table layout come
 * from libyaul bup/bup.h and bup.c (6012f79f; Copyright (c) Israel Jacquez,
 * MIT, see libyaul LICENSE); its wrappers and bup_init are not used.
 * SATURN_SAVE_HOST turns the two BIOS pointer reads into fixture calls.
 */
#include "saturn_save.h" /* <bup.h> before system.h's open/read/... macros */
#include "saturn_pad.h"
#include <libapi.h>
#include <string.h>

#define REC_HDR 36u     /* magic, version, header size, size, name */
#define PS1_BLOCK 8192u /* LibRef47 p0197 */
#define REC_MAX (REC_HDR + SATURN_SAVE_MAX_BLOCKS * PS1_BLOCK)
#define DIR_MAX 128     /* BUP_Dir table entries */
#define NAME_MAX_PS1 20 /* PS1 card name limit (nvom) */
#define DEV 0           /* built-in memory; cartridge never used */

#ifdef SATURN_SAVE_HOST
saturn_save_bios_init_t saturn_save_hw_bios_init(void);
bup_stub_t* saturn_save_hw_stub(void);
#define BIOS_INIT() saturn_save_hw_bios_init()
#define BIOS_STUB() saturn_save_hw_stub()
#else
/* 32-bit pointer words (bup.c:3-4, :15, :24) */
#define BIOS_INIT()                                                            \
    ((saturn_save_bios_init_t)(uintptr_t)(*(volatile uint32_t*)0x06000358u))
#define BIOS_STUB() ((bup_stub_t*)(uintptr_t)(*(volatile uint32_t*)0x06000354u))
#endif

/* the mirror's 2048 + 30 work words; bup_t is 16384 + 8192 B. */
static struct {
    bup_t b;
    uint32_t slack[30];
} bios_area;
static bup_devices_t devs;
static bup_stub_t* stub;
static int cause, guard_open, reset_open;

/* one static record, so one file open and at most 3 PS1 blocks per
 * file (36 + 24576 B); a bigger cap costs 8 KB of RAM per block. */
static uint8_t rec[REC_MAX] __attribute__((aligned(4)));
static struct {
    int open, mode;
    uint32_t size, pos;
    char key[12];
} f;

/* 128 directory entries static (36 B each); more records on the
 * device than that -> firstfile/format fail with SATURN_SAVE_E_DIR. */
static bup_directory_t dirtab[DIR_MAX];
static struct {
    char name[NAME_MAX_PS1 + 1];
    uint32_t size;
} snap[DIR_MAX];
static int snap_n, snap_i;

static int fail(int c) {
    cause = c;
    return -1;
}

static void put16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t* p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}
static uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t* p) {
    return get16(p) << 16 | get16(p + 2);
}

/* Reset guard: RESDISA timeout -> no BIOS call. RESENAB timeout -> the BIOS
 * result stands, counted, retried at the next save call. */
static int guard_on(void) {
    if (saturn_smpc_resdisa())
        return fail(SATURN_SAVE_E_RESDISA);
    reset_open = 1;
    return 0;
}
static void guard_off(void) {
    if (saturn_smpc_resenab())
        guard_open++;
    else
        reset_open = 0;
}
static void retry_resenab(void) {
    if (reset_open)
        guard_off();
}

/* BIOS init at the first use (zeroed devices, device 0 only). */
static int bios(void) {
    if (stub)
        return 0;
    if (guard_on())
        return -1;
    memset(&devs, 0, sizeof devs);
    BIOS_INIT()(bios_area.b.lib, bios_area.b.workarea, &devs);
    stub = BIOS_STUB();
    guard_off();
    return stub ? 0 : fail(BUP_NON);
}

/* Device 0 present and formatted (NON, UNFORMAT = an absent card). */
static int device(void) {
    bup_stat_t st;
    int s;
    if (bios())
        return -1;
    s = stub->op_status(DEV, REC_HDR + PS1_BLOCK, &st);
    return s ? fail(s) : 0;
}

/* "bu00:" -> 0, "bu10:" -> 1 (absent card), else -1 */
static int path_dev(const char* p) {
    if (p && strncmp(p, "bu00:", 5) == 0)
        return 0;
    if (p && strncmp(p, "bu10:", 5) == 0)
        return 1;
    return -1;
}

static int valid_name(const char* n) {
    size_t len = strlen(n), i;
    if (len < 12 || len > NAME_MAX_PS1)
        return 0;
    for (i = 0; i < len; i++)
        if (n[i] < 0x21 || n[i] > 0x7E || n[i] == '*' || n[i] == '?')
            return 0;
    for (i = 2; i <= 5; i++)
        if (n[i] < 'A' || n[i] > 'Z')
            return 0;
    for (i = 7; i <= 11; i++)
        if (n[i] < '0' || n[i] > '9')
            return 0;
    return 1;
}

/* BIOS key: name[3..5] + name[7..11] + last 3 of name[12..], '_'-padded */
static void make_key(const char* n, char key[12]) {
    size_t g = strlen(n) - 12, at = g > 3 ? g - 3 : 0, i;
    memcpy(key, n + 3, 3);
    memcpy(key + 3, n + 7, 5);
    for (i = 0; i < 3; i++) {
        char c = at + i < g ? n[12 + at + i] : '_';
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')))
            c = '_';
        key[8 + i] = c;
    }
    key[11] = 0;
}

/* Name valid on bu00: and device ready -> 0 with the key; else -1. */
static int prepare(const char* path, char key[12]) {
    if (path_dev(path) != 0 || !valid_name(path + 5))
        return fail(SATURN_SAVE_E_ARG);
    if (device())
        return -1;
    make_key(path + 5, key);
    return 0;
}

static int ours(uint32_t data_size) {
    return data_size >= REC_HDR && memcmp(rec, "PSYZ", 4) == 0 &&
           get16(rec + 6) == REC_HDR && rec[12 + 23] == 0 &&
           get32(rec + 8) == data_size - REC_HDR;
}

/* Read a directory entry's record into rec when it fits: 1 = ours, 0 =
 * foreign or not read, -1 = BIOS error. Never reads an oversize record. */
static int load_entry(const bup_directory_t* d) {
    int s;
    if (d->data_size < REC_HDR || d->data_size > REC_MAX)
        return 0;
    s = stub->op_read(DEV, d->filename, rec);
    if (s)
        return fail(s);
    return ours((uint32_t)d->data_size);
}
/* BUP_Read has no size argument: the key's directory entry first. 1 = the
 * exact 11-char filename found, 0 = none, -1 = BIOS error. */
static int find(const char* key, bup_directory_t* d) {
    int32_t n = stub->op_dir(DEV, key, 1, d);
    if (n < 0)
        return fail(SATURN_SAVE_E_DIR);
    return n > 0 && strncmp(d->filename, key, 11) == 0;
}

static int commit(uint8_t wmode) {
    bup_directory_t d;
    int s;
    memset(&d, 0, sizeof d);
    memcpy(d.filename, f.key, 12);
    memcpy(d.comment, "PSYZPS1", 8);
    d.language = BUP_LANG_ENGLISH;
    d.date = 0;
    d.data_size = REC_HDR + f.size;
    if (guard_on())
        return -1;
    s = stub->op_write(DEV, &d, rec, wmode);
    guard_off();
    if (s)
        return fail(s);
    s = stub->op_verify(DEV, f.key, rec);
    return s ? fail(s) : 0;
}

int open(const char* devname, int flag) {
    bup_directory_t d;
    char key[12];
    const char* name;
    int r;
    retry_resenab();
    if (f.open)
        return fail(SATURN_SAVE_E_BUSY);
    if (prepare(devname, key))
        return -1;
    name = devname + 5;
    if ((r = find(key, &d)) < 0)
        return -1;
    if (flag & FCREAT) {
        uint32_t blocks = (uint32_t)flag >> 16;
        if (blocks == 0)
            blocks = 1; /* nvom: 0 taken as 1 */
        if (blocks > SATURN_SAVE_MAX_BLOCKS)
            return fail(SATURN_SAVE_E_CAP);
        if (r)
            return fail(BUP_FOUND); /* existing key (ours or not) refused */
        memset(rec, 0, REC_HDR + blocks * PS1_BLOCK);
        memcpy(rec, "PSYZ", 4);
        put16(rec + 4, 1);
        put16(rec + 6, REC_HDR);
        put32(rec + 8, blocks * PS1_BLOCK);
        memcpy(rec + 12, name, strlen(name));
        memcpy(f.key, key, 12);
        f.size = blocks * PS1_BLOCK;
        if (commit(BUP_WRITE_MODE_PROTECT))
            return -1;
    } else {
        if (!r)
            return fail(BUP_NOT_FOUND);
        if ((r = load_entry(&d)) < 0)
            return -1;
        if (r == 0 || strncmp((const char*)rec + 12, name, 24) != 0)
            return fail(SATURN_SAVE_E_FOREIGN);
        memcpy(f.key, key, 12);
        f.size = get32(rec + 8);
    }
    f.open = 1;
    f.mode = flag & (FREAD | FWRITE);
    f.pos = 0;
    return SATURN_SAVE_FD;
}

static int bad_fd(long fd, int need) {
    return fd != SATURN_SAVE_FD || !f.open || (f.mode & need) != need;
}

int close(int fd) {
    retry_resenab();
    if (bad_fd(fd, 0))
        return fail(SATURN_SAVE_E_ARG);
    f.open = 0;
    return fd;
}

long lseek(long fd, long offset, long flag) {
    long t;
    retry_resenab();
    if (bad_fd(fd, 0) || (flag != SEEK_SET && flag != SEEK_CUR))
        return fail(SATURN_SAVE_E_ARG);
    t = flag == SEEK_SET ? offset : (long)f.pos + offset;
    if (t < 0 || t > (long)f.size)
        return fail(SATURN_SAVE_E_ARG);
    f.pos = (uint32_t)t;
    return t;
}

long read(long fd, void* buf, long n) {
    retry_resenab();
    if (bad_fd(fd, FREAD) || n < 0)
        return fail(SATURN_SAVE_E_ARG);
    if (n > (long)(f.size - f.pos))
        n = (long)(f.size - f.pos);
    memcpy(buf, rec + REC_HDR + f.pos, (size_t)n);
    f.pos += (uint32_t)n;
    return n;
}

long write(long fd, void* buf, long n) {
    retry_resenab();
    if (bad_fd(fd, FWRITE) || n < 0 || n > (long)(f.size - f.pos))
        return fail(SATURN_SAVE_E_ARG);
    if (n == 0)
        return 0;
    memcpy(rec + REC_HDR + f.pos, buf, (size_t)n);
    if (commit(BUP_WRITE_MODE_OVERWRITE))
        return -1;
    f.pos += (uint32_t)n;
    return n;
}

/* LibRef47 p0035: '?' one character, '*' any rest (characters after it
 * ignored) */
static int match(const char* p, const char* n) {
    for (; *p; p++, n++) {
        if (*p == '*')
            return 1;
        if (!*n || (*p != '?' && *p != *n))
            return 0;
    }
    return !*n;
}

static struct DIRENTRY* fill(struct DIRENTRY* e) {
    if (snap_i >= snap_n)
        return NULL;
    memset(e, 0, sizeof *e);
    memcpy(e->name, snap[snap_i].name, 19); /* 19 chars + NUL */
    e->attr = 0x50;
    e->size = (int)snap[snap_i].size;
    snap_i++;
    return e;
}

/* All device records: count, or -1 (cause latched) */
static int32_t dir_all(void) {
    int32_t n;
    if (device())
        return -1;
    n = stub->op_dir(DEV, "", DIR_MAX, dirtab);
    return n < 0 ? fail(SATURN_SAVE_E_DIR) : n;
}

struct DIRENTRY* my_firstfile(char* dirPath, struct DIRENTRY* firstEntry) {
    const char* pat;
    int32_t n, i;
    int j;
    retry_resenab();
    snap_n = snap_i = 0;
    if (f.open) {
        fail(SATURN_SAVE_E_BUSY);
        return NULL;
    }
    if (path_dev(dirPath) != 0) {
        fail(SATURN_SAVE_E_ARG);
        return NULL;
    }
    pat = dirPath[5] ? dirPath + 5 : "*";
    if ((n = dir_all()) < 0)
        return NULL;
    for (i = 0; i < n; i++) {
        const char* nm = (const char*)rec + 12;
        if (load_entry(&dirtab[i]) != 1 || !match(pat, nm))
            continue;
        /* insertion sort by strcmp, at most 128 names */
        for (j = snap_n; j > 0 && strcmp(snap[j - 1].name, nm) > 0; j--)
            snap[j] = snap[j - 1];
        memset(snap[j].name, 0, sizeof snap[j].name);
        strncpy(snap[j].name, nm, NAME_MAX_PS1);
        snap[j].size = get32(rec + 8);
        snap_n++;
    }
    return fill(firstEntry);
}

struct DIRENTRY* my_nextfile(struct DIRENTRY* outEntry) {
    retry_resenab();
    return fill(outEntry);
}

static int delete_key(const char* key) {
    int s;
    if (guard_on())
        return -1;
    s = stub->op_delete(DEV, key);
    guard_off();
    return s ? fail(s) : 0;
}

long my_erase(char* path) {
    bup_directory_t d;
    char key[12];
    retry_resenab();
    if (f.open) {
        fail(SATURN_SAVE_E_BUSY);
        return 0;
    }
    if (prepare(path, key) || find(key, &d) != 1 || load_entry(&d) != 1 ||
        strncmp((const char*)rec + 12, path + 5, 24) != 0)
        return 0;
    return delete_key(key) == 0;
}

long my_format(char* fs) {
    int32_t n, i;
    retry_resenab();
    if (f.open) {
        fail(SATURN_SAVE_E_BUSY);
        return 0;
    }
    if (path_dev(fs) != 0 || (n = dir_all()) < 0)
        return 0;
    for (i = 0; i < n; i++)
        if (load_entry(&dirtab[i]) == 1 && delete_key(dirtab[i].filename))
            return 0;
    return 1;
}

/* LibRef47 p0179; nothing to do: the BIOS is initialised at the first use. */
void _bu_init(void) {}

int saturn_save_stat(uint32_t datasize, bup_stat_t* out) {
    retry_resenab();
    if (bios())
        return -1;
    return stub->op_status(DEV, datasize, out);
}

int saturn_save_cause(void) { return cause; }
int saturn_save_guard_open(void) { return guard_open; }
