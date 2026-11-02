#include <stdint.h>
#include <string.h>
#include <psyz.h>
#include <libspu.h>
#include <libsnd.h>

#include "ztest.h"

void Psyz_KernelVBlank(void);

enum { kKeyOn, kKeyOff, kBend };

typedef struct {
    int call;
    int kind;
    int prog;
    int note;
    int vel;
    int vol;
    int pan;
    unsigned short pitch;
} SeqRow;

// 480 ticks per quarter at 120 BPM: 16 ticks per SS_TICK60 call
#define SEQ_HEADER                                                             \
    'p', 'Q', 'E', 'S', 0x00, 0x00, 0x00, 0x01, 0x01, 0xE0, 0x07, 0xA1, 0x20,  \
        0x04, 0x02

static const unsigned char seq_note_on[] = {
    SEQ_HEADER, 8, 0x90, 60, 64, 16, 0x90, 64, 96, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_note_on[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOn, 0, 64, 96, 127, 64, 0x1428},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
    {5, kKeyOff, 0, 64, 0, 0, 0, 0},
};

static const unsigned char seq_note_off_9n[] = {
    SEQ_HEADER, 8, 0x90, 60, 64, 16, 0x90, 60, 0, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_note_off_9n[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOff, 0, 60, 0, 0, 0, 0},
};

static const unsigned char seq_note_off_8n[] = {
    SEQ_HEADER, 8,    0x90, 60, 64, 16,   0x90, 62,   64,
    0,          0x80, 60,   0,  32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_note_off_8n[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOff, 0, 60, 0, 0, 0, 0},
    {3, kKeyOn, 0, 62, 64, 127, 64, 0x11F5},
    {5, kKeyOff, 0, 62, 0, 0, 0, 0},
};

static const unsigned char seq_running_status[] = {
    SEQ_HEADER, 8, 0x90, 60, 64, 16, 62,   80,   16,
    60,         0, 16,   62, 0,  16, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_running_status[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOn, 0, 62, 80, 127, 64, 0x11F5},
    {4, kKeyOff, 0, 60, 0, 0, 0, 0},
    {5, kKeyOff, 0, 62, 0, 0, 0, 0},
};

static const unsigned char seq_program_change[] = {
    SEQ_HEADER, 8, 0xC0, 1, 16, 0x90, 60, 64, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_program_change[] = {
    {3, kKeyOn, 1, 60, 64, 127, 64, 0x2000},
    {5, kKeyOff, 1, 60, 0, 0, 0, 0},
};

static const unsigned char seq_main_volume[] = {
    SEQ_HEADER, 8, 0xB0, 7, 64, 16, 0x90, 60, 127, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_main_volume[] = {
    {3, kKeyOn, 0, 60, 127, 64, 64, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

static const unsigned char seq_panpot[] = {
    SEQ_HEADER, 8, 0xB0, 10, 32, 16, 0x90, 60, 127, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_panpot[] = {
    {3, kKeyOn, 0, 60, 127, 127, 32, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

static const unsigned char seq_pitch_bend[] = {
    SEQ_HEADER, 8,    0x90, 60,   64, 16,   0xE0, 0,    127,
    16,         0xE0, 0,    0x40, 16, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_pitch_bend[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kBend, 0, 60, 0, 0, 0, 0x11F5},
    {4, kBend, 0, 60, 0, 0, 0, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// FF 51 to 60 BPM: 8 ticks per call from the call that reads it
static const unsigned char seq_tempo[] = {
    SEQ_HEADER, 8,  0x90, 60, 64,   12, 0xFF, 0x51, 0x0F, 0x42, 0x40, 8,
    0x90,       62, 64,   8,  0x90, 64, 64,   8,    0xFF, 0x2F, 0x00,
};
static const SeqRow want_tempo[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {4, kKeyOn, 0, 62, 64, 127, 64, 0x11F5},
    {5, kKeyOn, 0, 64, 64, 127, 64, 0x1428},
    {6, kKeyOff, 0, 60, 0, 0, 0, 0},
    {6, kKeyOff, 0, 62, 0, 0, 0, 0},
    {6, kKeyOff, 0, 64, 0, 0, 0, 0},
};

static const unsigned char seq_end[] = {
    SEQ_HEADER, 8, 0x90, 60, 64, 16, 0xFF, 0x2F, 0x00,
};
static const SeqRow want_end[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// Controllers below have no seq.py IR rule, hence the ev_ and rows_ names

// CC 0 selects VAB 1 (not open): note 60 is dropped; back on VAB 0, 62 plays
static const unsigned char ev_bank_change[] = {
    SEQ_HEADER, 8, 0xB0, 0,    1,  0,  0x90, 60,   64,   16,   0xB0,
    0,          0, 0,    0x90, 62, 64, 32,   0xFF, 0x2F, 0x00,
};
static const SeqRow rows_bank_change[] = {
    {3, kKeyOn, 0, 62, 64, 127, 64, 0x11F5},
    {5, kKeyOff, 0, 62, 0, 0, 0, 0},
};

// RPN 0/0 (CC 101, CC 100) then CC 6 12: the bend range becomes 12
static const unsigned char ev_data_entry[] = {
    SEQ_HEADER, 8,    0xB0, 101, 0,  0,    0xB0, 100, 0,  0,    0xB0, 6,    12,
    0,          0x90, 60,   64,  16, 0xE0, 0,    127, 16, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_data_entry[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kBend, 0, 60, 0, 0, 0, 0x2000},
    {4, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// CC 6 with no parameter number pending only reads the next delta (16)
static const unsigned char ev_data_entry_delta[] = {
    SEQ_HEADER, 8, 0xB0, 6, 5, 16, 0x90, 60, 64, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_data_entry_delta[] = {
    {3, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// CC 11 64 sets the program volume: velocity 127 then sounds as volume 64
static const unsigned char ev_expression[] = {
    SEQ_HEADER, 8, 0xB0, 11, 64, 16, 0x90, 60, 127, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_expression[] = {
    {3, kKeyOn, 0, 60, 127, 64, 64, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// CC 6 acts only once both parameter numbers are in: CC 100 alone and CC 98
// alone are ignored; CC 101 completes the RPN, CC 99 the NRPN (tone 0 max 61)
static const unsigned char ev_nrpn_rpn[] = {
    SEQ_HEADER, 8,  0xB0, 100, 0,  0,  0xB0, 6,    12,   0,  0xB0, 98,
    3,          0,  0xB0, 6,   61, 0,  0x90, 60,   64,   16, 0xE0, 0,
    127,        16, 0xB0, 101, 0,  0,  0xB0, 6,    12,   0,  0xE0, 0,
    127,        16, 0xB0, 99,  0,  0,  0xB0, 6,    61,   0,  0x90, 60,
    0,          0,  0x90, 62,  64, 16, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_nrpn_rpn[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kBend, 0, 60, 0, 0, 0, 0x11F5},
    {4, kBend, 0, 60, 0, 0, 0, 0x2000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// CC 121 after CC 7 64: the channel volume is back to 127
static const unsigned char ev_reset_all[] = {
    SEQ_HEADER, 8,    0xB0, 7,  64, 0,    0xB0, 121,  0,
    16,         0x90, 60,   64, 32, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_reset_all[] = {
    {3, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// NRPN tone 0 attribute 2 (lower note limit) 61: 60 is dropped, 62 plays
static const unsigned char ev_nrpn_vab_attr[] = {
    SEQ_HEADER, 8,  0xB0, 99, 0,    0,  0xB0, 98,   2,    0,
    0xB0,       6,  61,   0,  0x90, 60, 64,   0,    0x90, 62,
    64,         16, 0x90, 62, 0,    16, 0xFF, 0x2F, 0x00,
};
static const SeqRow rows_nrpn_vab_attr[] = {
    {2, kKeyOn, 0, 62, 64, 127, 64, 0x11F5},
    {3, kKeyOff, 0, 62, 0, 0, 0, 0},
};

// NRPN loop: CC 99 20 starts it, CC 6 sets the count, CC 99 30 ends it; a
// pass is 32 ticks (two calls)
#define LOOP_SEQ(count)                                                        \
    SEQ_HEADER, 8, 0xB0, 99, 20, 0, 0xB0, 6, count, 0, 0x90, 60, 64, 12, 0x90, \
        60, 0, 20, 0xB0, 99, 30, 0, 0xFF, 0x2F, 0x00

static const unsigned char ev_loop_twice[] = {LOOP_SEQ(2)};
static const SeqRow rows_loop_twice[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {3, kKeyOff, 0, 60, 0, 0, 0, 0},
    {4, kKeyOn, 0, 60, 64, 127, 64, 0x1000},
    {5, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// count 127 loops forever
static const unsigned char ev_loop_forever[] = {LOOP_SEQ(127)};
static const SeqRow rows_loop_forever[] = {
    {2, kKeyOn, 0, 60, 64, 127, 64, 0x1000}, {3, kKeyOff, 0, 60, 0, 0, 0, 0},
    {4, kKeyOn, 0, 60, 64, 127, 64, 0x1000}, {5, kKeyOff, 0, 60, 0, 0, 0, 0},
    {6, kKeyOn, 0, 60, 64, 127, 64, 0x1000}, {7, kKeyOff, 0, 60, 0, 0, 0, 0},
    {8, kKeyOn, 0, 60, 64, 127, 64, 0x1000}, {9, kKeyOff, 0, 60, 0, 0, 0, 0},
};

// SEP version 0 holding one sequence: id 0, the SEQ_HEADER fields, data size
static const unsigned char ev_sep_single[] = {
    'p',  'Q',  'E',  'S',  0x00, 0x00, 0x00, 0x00, 0x01,
    0xE0, 0x07, 0xA1, 0x20, 0x04, 0x02, 0x00, 0x00, 0x00,
    0x08, 8,    0x90, 60,   64,   16,   0xFF, 0x2F, 0x00,
};

// CC 64 127: the damper adds 2 to the tone's ADSR2 at key on
static const unsigned char ev_damper[] = {
    SEQ_HEADER, 8, 0xB0, 64, 127, 0, 0x90, 60, 64, 16, 0xFF, 0x2F, 0x00,
};

// two programs of one tone each on one looping silent VAG
static struct {
    VabHdr hdr;
    ProgAtr prog[128];
    VagAtr tone[2 * 16];
    unsigned short vag[256];
} vab_head;
static unsigned char vab_body[32];
static char seq_table[SS_SEQ_TABSIZ];
static short vab;

static void make_vab(void) {
    int i;

    memset(&vab_head, 0, sizeof(vab_head));
    vab_head.hdr.form = 0x56414270; // "pBAV"
    vab_head.hdr.ver = 7;
    vab_head.hdr.fsize = sizeof(vab_head) + sizeof(vab_body);
    vab_head.hdr.ps = 2;
    vab_head.hdr.ts = 2;
    vab_head.hdr.vs = 1;
    vab_head.hdr.mvol = 127;
    vab_head.hdr.pan = 64;
    for (i = 0; i < 2; i++) {
        VagAtr* t = &vab_head.tone[i * 16];
        vab_head.prog[i].tones = 1;
        vab_head.prog[i].mvol = 127;
        vab_head.prog[i].mpan = 64;
        t->prior = 8;
        t->vol = 127;
        t->pan = 64;
        t->center = i ? 48 : 60;
        t->max = 127;
        t->pbmin = 2;
        t->pbmax = 2;
        t->adsr1 = 0x000F;
        t->adsr2 = 0x1FC0;
        t->prog = i;
        t->vag = 1;
    }
    vab_head.vag[1] = sizeof(vab_body) / 8;
    memset(vab_body, 0, sizeof(vab_body));
    vab_body[1] = 0x04;
    vab_body[17] = 0x03;
}

ZTEST_SETUP(snd_seq) {
    Psyz_AudioPause();
    Psyz_AudioLock();
    Psyz_SpuInit();
    Psyz_SpuReset(0);
    SsInit();
    SsSetTableSize(seq_table, 1, 1);
    SsSetTickMode(SS_TICK60);
    make_vab();
    vab = SsVabOpenHeadSticky((u_char*)&vab_head, -1, 0x1010);
    zassert_s32_ge(0, vab);
    zassert_s32_eq(vab, SsVabTransBody(vab_body, vab));
    SsVabTransCompleted(SS_WAIT_COMPLETED);
}

ZTEST_TEARDOWN(snd_seq) {
    SsVabClose(vab);
    Psyz_AudioUnlock();
    Psyz_AudioUnpause();
}

static void spu_run(void) {
#ifndef __psx__
    short frames[32 * 2];
    Psyz_SpuPullSamples(frames, 32);
#endif
}

// libsnd's key-on volume for a VAB, program and tone all at 127 and centred
static void key_on_volume(const SeqRow* row, short* l, short* r) {
    unsigned left = row->vel * row->vol / 127 * 129;
    unsigned right = left;

    if (row->pan < 64) {
        right = right * row->pan / 63;
    } else {
        left = left * (127 - row->pan) / 63;
    }
    *l = (short)(left * left / 16383);
    *r = (short)(right * right / 16383);
}

static void run_rows(
    short acc, void (*tick)(void), const SeqRow* want, int n, int calls) {
    int keyed[24] = {0};
    int note[24] = {0};
    unsigned short pitch[24] = {0};
    int call, v, on, got = 0;
    unsigned short p;
    short l, r, want_l, want_r;

    for (call = 1; call <= calls; call++) {
        tick();
        spu_run();
        for (v = 0; v < 24; v++) {
            on = SpuGetKeyStatus(1 << v) != 0;
            p = Psyz_SpuRead(v * 16 + 4);
            if (on == keyed[v] && (!on || p == pitch[v])) {
                continue;
            }
            zassert_s32_lt(n, got);
            zassert_s32_eq(want[got].call, call);
            if (on && !keyed[v]) {
                zassert_s32_eq(want[got].kind, kKeyOn);
                zassert_u16_eq(want[got].pitch, p);
                SpuGetVoiceVolume(v, &l, &r);
                key_on_volume(&want[got], &want_l, &want_r);
                zassert_s16_eq(want_l, l);
                zassert_s16_eq(want_r, r);
                note[v] = want[got].note;
            } else if (on) {
                zassert_s32_eq(want[got].kind, kBend);
                zassert_s32_eq(want[got].note, note[v]);
                zassert_u16_eq(want[got].pitch, p);
            } else {
                zassert_s32_eq(want[got].kind, kKeyOff);
                zassert_s32_eq(want[got].note, note[v]);
            }
            keyed[v] = on;
            pitch[v] = p;
            got++;
        }
    }
    zassert_s32_eq(n, got);
}

static void play_seq(
    const unsigned char* seq, const SeqRow* want, int n, int calls) {
    short acc = SsSeqOpen((u_long*)seq, vab);

    zassert_s32_ge(0, acc);
    SsSeqPlay(acc, SSPLAY_PLAY, 1);
    run_rows(acc, SsSeqCalledTbyT, want, n, calls);
    zassert_s32_eq(0, SsIsEos(acc, 0));
    SsSeqClose(acc);
}

#define PLAY(name, calls)                                                      \
    play_seq(                                                                  \
        seq_##name, want_##name, sizeof(want_##name) / sizeof(SeqRow), calls)

#define PLAY_EV(name, calls)                                                   \
    play_seq(                                                                  \
        ev_##name, rows_##name, sizeof(rows_##name) / sizeof(SeqRow), calls)

ZTEST(snd_seq, note_on) { PLAY(note_on, 8); }

ZTEST(snd_seq, note_off_9n) { PLAY(note_off_9n, 8); }

ZTEST(snd_seq, note_off_8n) { PLAY(note_off_8n, 8); }

ZTEST(snd_seq, running_status) { PLAY(running_status, 8); }

ZTEST(snd_seq, program_change) { PLAY(program_change, 8); }

ZTEST(snd_seq, main_volume) { PLAY(main_volume, 8); }

ZTEST(snd_seq, panpot) { PLAY(panpot, 8); }

ZTEST(snd_seq, pitch_bend) { PLAY(pitch_bend, 8); }

ZTEST(snd_seq, tempo) { PLAY(tempo, 8); }

ZTEST(snd_seq, end_of_sequence) { PLAY(end, 8); }

ZTEST(snd_seq, bank_change) {
    zassert_s32_eq(0, vab);
    PLAY_EV(bank_change, 8);
}

ZTEST(snd_seq, data_entry) { PLAY_EV(data_entry, 8); }

ZTEST(snd_seq, data_entry_delta) { PLAY_EV(data_entry_delta, 8); }

ZTEST(snd_seq, expression) { PLAY_EV(expression, 8); }

ZTEST(snd_seq, nrpn_rpn) { PLAY_EV(nrpn_rpn, 8); }

ZTEST(snd_seq, reset_all) { PLAY_EV(reset_all, 8); }

ZTEST(snd_seq, nrpn_vab_attr) { PLAY_EV(nrpn_vab_attr, 8); }

ZTEST(snd_seq, loop_twice) { PLAY_EV(loop_twice, 9); }

// still playing after four passes: SsIsEos reads the play mode, 1
ZTEST(snd_seq, loop_forever) {
    short acc = SsSeqOpen((u_long*)ev_loop_forever, vab);

    zassert_s32_ge(0, acc);
    SsSeqPlay(acc, SSPLAY_PLAY, 1);
    run_rows(acc, SsSeqCalledTbyT, rows_loop_forever, 8, 9);
    zassert_s32_eq(1, SsIsEos(acc, 0));
    SsSeqClose(acc);
}

ZTEST(snd_seq, damper) {
    short acc = SsSeqOpen((u_long*)ev_damper, vab);
    int v;

    zassert_s32_ge(0, acc);
    SsSeqPlay(acc, SSPLAY_PLAY, 1);
    SsSeqCalledTbyT();
    SsSeqCalledTbyT();
    spu_run();
    for (v = 0; v < 24 && !SpuGetKeyStatus(1 << v); v++) {
    }
    zassert_s32_lt(24, v);
    zassert_u16_eq(0x1FC0 + 2, Psyz_SpuRead(v * 16 + 10));
    SsSeqClose(acc);
}

ZTEST(snd_seq, sep_single) {
    short acc = SsSepOpen((u_long*)ev_sep_single, vab, 1);

    zassert_s32_ge(0, acc);
    SsSepPlay(acc, 0, SSPLAY_PLAY, 1);
    run_rows(acc, SsSeqCalledTbyT, want_end, 2, 8);
    zassert_s32_eq(0, SsIsEos(acc, 0));
    SsSepClose(acc);
}

// SS_TICK60 on NTSC: SsStart hooks IRQ 0, so each VBlank runs SsSeqCalledTbyT
ZTEST(snd_seq, tick60_vblank) {
    short acc = SsSeqOpen((u_long*)seq_note_on, vab);

    zassert_s32_ge(0, acc);
    SsSeqPlay(acc, SSPLAY_PLAY, 1);
    SsStart();
    run_rows(acc, Psyz_KernelVBlank, want_note_on, 4, 8);
    SsEnd();
    zassert_s32_eq(0, SsIsEos(acc, 0));
    SsSeqClose(acc);
}
