#include <psyz.h>
#include <libspu.h>
#include <libsnd.h>
#include <psyz/log.h>
#include "../../decomp/src/libspu/libspu_private.h"
#include "../../decomp/src/libsnd/libsnd_private.h"

#define LEN(x) ((s32)(sizeof(x) / sizeof(*(x))))
#define NUM_VOICES 24

static unsigned long long psyz_seq_note_on_count;
static unsigned long long psyz_seq_voice_start_count;
static unsigned long long psyz_snd_pitch_update_count;

unsigned long long Psyz_SeqNoteOnCount(void) { return psyz_seq_note_on_count; }

unsigned long long Psyz_SeqVoiceStartCount(void) {
    return psyz_seq_voice_start_count;
}

unsigned long long Psyz_SndPitchUpdateCount(void) {
    return psyz_snd_pitch_update_count;
}

typedef void (*SndSsMarkCallbackProc)(short seq_no, short sep_no, short data);

extern short _snd_seq_s_max;
extern short _snd_seq_t_max;
extern int _snd_ev_flag;
extern _SsFCALL SsFCALL;
extern SndSsMarkCallbackProc _SsMarkCallback[32][16];
extern unsigned int VBLANK_MINUS;
extern int _snd_openflag;

static void SetVoiceData(int nVoice, unsigned short* data) {
    for (int i = 0; i < 8; i++) {
        Psyz_SpuWrite(nVoice * 0x10 + i * 2, data[i]);
    }
}

static void SetStateData(unsigned short* data, unsigned nWords) {
    for (unsigned i = 0; i < nWords; i++) {
        Psyz_SpuWrite(0x180 + i * 2, data[i]);
    }
}

static unsigned short default_voice[] = {0, 0, 0x1000, 0x3000, 0x00BF, 0, 0, 0};
static unsigned short default_state[] = {
    0x3FFF, 0x3FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
extern SPU_RXX* _svm_sreg;

void _SsInit(void) {
    int i, j;

    _svm_sreg = (SPU_RXX*)_spu_RXX;
    for (i = 0; i < NUM_VOICES; i++) {
        SetVoiceData(i, default_voice);
    }
    SetStateData(default_state, LEN(default_state));

    _SsVmInit(NUM_VOICES);
    for (j = 0; j < LEN(_SsMarkCallback); j++) {
        for (i = 0; i < LEN(*_SsMarkCallback); i++) {
            _SsMarkCallback[j][i] = NULL;
        }
    }

    VBLANK_MINUS = 60;
    _snd_openflag = 0;
    _snd_ev_flag = 0;
}

// void _SsVmFlush(void) { NOT_IMPLEMENTED; }

void SsSetMarkCallback(
    short access_num, short seq_num, SsMarkCallbackProc proc) {
    NOT_IMPLEMENTED;
}

void SsSeqSetDecrescendo(short seq_access_num, short vol, long v_time) {
    NOT_IMPLEMENTED;
}

void _SsVmGetSeqVol(short seq_sep_no, short* voll, short* volr) {
    NOT_IMPLEMENTED;
}

static short set_voice_pitch(
    short voice, short vabId, short prog, short note, short fine) {
    if (voice < 0 || voice >= NUM_VOICES || _svm_voice[voice].vabId != vabId ||
        _svm_voice[voice].prog != prog || _SsVmVSetUp(vabId, prog) != 0) {
        if (getenv("PSYZ_SND_PITCH_TRACE"))
            fprintf(stderr,
                    "set_voice_pitch reject voice=%d vab=%d(cur %d) "
                    "prog=%d(cur %d)\n",
                    voice, vabId,
                    voice >= 0 && voice < NUM_VOICES ? _svm_voice[voice].vabId
                                                     : -99,
                    prog,
                    voice >= 0 && voice < NUM_VOICES ? _svm_voice[voice].prog
                                                     : -99);
        return -1;
    }
    _svm_cur.tone = _svm_voice[voice].tone;
    _svm_cur.fake_program = _svm_voice[voice].fake_program;
    _svm_sreg_buf[voice].pitch = note2pitch2(note, fine);
    _svm_sreg_dirty[voice] |= 4;
    /* PsyQ's SsUtChangePitch only writes the pitch register; the voice
     * keeps its key-on note. Rage Racer relies on that: it always passes
     * old_note 0x3C (the note it keyed with), so recording new_note here
     * made the very next update fail and froze every pitched effect
     * (engine revs, skids, impacts) after its first change. */
    _svm_voice[voice].unk04 = _svm_sreg_buf[voice].pitch;
    psyz_snd_pitch_update_count++;
    return 0;
}

short SsUtChangePitch(short voice, short vabId, short prog, short old_note,
                      short old_fine, short new_note, short new_fine) {
    (void)old_fine;
    if (voice < 0 || voice >= NUM_VOICES ||
        _svm_voice[voice].note != old_note) {
        if (getenv("PSYZ_SND_PITCH_TRACE"))
            fprintf(
                stderr, "SsUtChangePitch reject voice=%d old_note=%d(cur %d)\n",
                voice, old_note,
                voice >= 0 && voice < NUM_VOICES ? _svm_voice[voice].note
                                                 : -99);
        return -1;
    }
    return set_voice_pitch(voice, vabId, prog, new_note, new_fine);
}

short SsUtPitchBend(
    short voice, short vabId, short prog, short note, short pbend) {
    int bend;
    int scaled;
    int base;
    int fine;
    int tone_index;
    (void)note;
    if (voice < 0 || voice >= NUM_VOICES ||
        _svm_voice[voice].seq_sep_no != 0x21 ||
        _svm_voice[voice].vabId != vabId || _svm_voice[voice].prog != prog ||
        _SsVmVSetUp(vabId, prog) != 0)
        return -1;

    /* Match PsyQ's asymmetric bend calculation: 64 is centre, and each
     * side is scaled by the selected tone's VAB bend limit. */
    tone_index = _svm_voice[voice].fake_program * 16 + _svm_voice[voice].tone;
    bend = pbend - 64;
    base = _svm_voice[voice].note;
    fine = 0;
    if (bend > 0) {
        scaled = bend * _svm_tn[tone_index].pbmax;
        base += scaled / 63;
        fine = (scaled % 63) * 2;
    } else if (bend < 0) {
        scaled = bend * _svm_tn[tone_index].pbmin;
        /* Arithmetic division toward minus infinity, as in the MIPS code. */
        int quotient = scaled >= 0 ? scaled / 64 : -((-scaled + 63) / 64);
        base += quotient - 1;
        fine = (scaled - quotient * 64) * 2 + 127;
    }
    return set_voice_pitch(voice, vabId, prog, base, fine);
}

void vmNoiseOn(char voice) { NOT_IMPLEMENTED; }

void SetAutoVol(short voice) { NOT_IMPLEMENTED; }

void SeAutoPan(short arg0, short arg1, short arg2, short arg3) {
    NOT_IMPLEMENTED;
}
