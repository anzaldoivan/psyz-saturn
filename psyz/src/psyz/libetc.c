#include <psyz.h>
#include <libetc.h>

#include "libapi.h"
#include "../internal.h"

#include <psyz/log.h>

// getScratchAddr()'s memory, aligned for any type a game keeps there.
_Alignas(64) unsigned char Psyz_Scratchpad[256 * sizeof(u_long)];

void MyPadInit(int mode);
void PadInit(int mode) { MyPadInit(mode); }

static unsigned UnpackPadBits(const char* buf) {
    if ((unsigned char)buf[0] != 0) {
        return 0; // port disconnected or frame not valid
    }
    switch ((unsigned char)buf[1]) {
    case PSYZ_CTRL_DIGITAL_PAD:
    case PSYZ_CTRL_ANALOG_STICK:
    case PSYZ_CTRL_ANALOG_PAD:
        break;
    default:
        return 0; // not a pad-like device, switches are not in bytes 2-3
    }
    unsigned int buttons0 = (unsigned char)buf[2];
    unsigned int buttons1 = (unsigned char)buf[3];
    unsigned int padq = (buttons0 << 8) | buttons1;
    return (~padq) & 0xFFFF;
}

unsigned int PadRead(int id) {
    char p0[PSYZ_PAD_BUF_LEN], p1[PSYZ_PAD_BUF_LEN];
    Psyz_PadsGet(0, p0, sizeof(p0));
    Psyz_PadsGet(1, p1, sizeof(p1));
    unsigned r = UnpackPadBits(p0) | (UnpackPadBits(p1) << 16);

    // PS1 gamepad cannot have opposite D-pad pressed at the same time
    // solves a bug on Castlevania SOTN when pressing both Left and Right
    // where its behavior is undefined.
    if (r & PADLleft && r & PADLright) {
        r &= ~(PADLleft | PADLright);
    }
    if (r & PADLup && r & PADLdown) {
        r &= ~(PADLup | PADLdown);
    }

    return r;
}

void PadStop(void) { NOT_IMPLEMENTED; }

static long video_mode = 0;
long GetVideoMode() { return video_mode; }
long SetVideoMode(long mode) {
    long prev = video_mode;
    video_mode = mode;
    return prev;
}

int InitTAP(char* bufA, long lenA, char* bufB, long lenB) {
    LOG_ONCE("forwarding to InitPAD");
    return InitPAD(bufA, lenA, bufB, lenB);
}

void StartTAP(void) {
    LOG_ONCE("forwarding to StartPAD");
    StartPAD();
}

static int pad_connected(int port, unsigned char* buf) {
    int index = port >> 4;
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    if ((port & 0x0F) || index < 0 || index > 1) {
        return 0;
    }
    Psyz_PadsGet(index, (char*)buf, 2);
    return buf[0] == 0 && buf[1] != 0xFF;
}

int PadGetState(int port) {
    // from PSY-Q 4.2
    unsigned char buf[2];
    // PadStateFindCTP1: the pad does not answer the 0x43 config command
    return pad_connected(port, buf) ? 2 : 0;
}

int PadInfoMode(int port, int term, int offs) {
    // from PSY-Q 4.2
    unsigned char buf[2];
    if (term != 1 || !pad_connected(port, buf)) {
        return 0;
    }
    return buf[1] >> 4;
}

void PadSetAct(int port, u_char* data, int len) {
    // from PSY-Q 4.2
}

int PadSetActAlign(int port, char* data) {
    // from PSY-Q 4.2
    return 0;
}

void PadInitDirect(u_char* pad1, u_char* pad2) {
    // from PSY-Q 4.2
    InitPAD((char*)pad1, PSYZ_PAD_BUF_LEN, (char*)pad2, PSYZ_PAD_BUF_LEN);
}

void PadInitMtap(u_char* pad1, u_char* pad2) {
    // from PSY-Q 4.2
    NOT_IMPLEMENTED;
}

void PadStartCom(void) {
    // from PSY-Q 4.2
}
