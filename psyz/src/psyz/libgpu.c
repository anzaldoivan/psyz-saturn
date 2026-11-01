#include <psyz.h>
#include <libgpu.h>
#include "../../../decomp/src/libgpu/libgpu_private.h"
#include <psyz/log.h>
#include "../draw.h"
#include "../internal.h"

// The GPU is a FIFO: the register reflects what it has consumed by the GPU the
// moment it's queried. The problem with the current PsyZ implementation is
// the GPU emulation is synchronous. GPU packets are accumulated in GPU_Enqueue
// then consumed by Psyz_GpuExeque on demand by the same thread. This means the
// values of GPU_STATUS will be stale, especially when 0xE1xxxxxx gets enqueued
// and then GPU_STATUS gets queried before Psyz_GpuExeque is called. But then
// calling Psyz_GpuExeque implies synchronization, which slows down performance.
static u32 GPU_STATUS = 0;

typedef enum {
    // https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-versions
    GPU_V0, // 1MB VRAM, retail
    GPU_V1, // 2MB VRAM, arcade
    GPU_V2, // 2MB VRAM, PSone
} GpuVersion;

static void GPU_clear_cache() { NOT_IMPLEMENTED; }

static void GPU_write_image() { NOT_IMPLEMENTED; }

static void GPU_read_image() { NOT_IMPLEMENTED; }

static int queue_len = 0;
static u_long queue_buf[0x4000];

typedef struct {
    PsyzGpuCommandHandler handler;
    void* userdata;
} UserGpuCommand;

static UserGpuCommand user_gpu_commands[256];

int Psyz_GpuRegisterCommandHandler(
    unsigned int opcode, PsyzGpuCommandHandler handler, void* userdata) {
    if (opcode > 0xFF) {
        return -1;
    }
    user_gpu_commands[opcode].handler = handler;
    user_gpu_commands[opcode].userdata = handler ? userdata : NULL;
    return 0;
}

int Psyz_GpuSetHorizontalGrid(
    unsigned int source_width, unsigned int target_width) {
    return Draw_SetHorizontalGrid(source_width, target_width);
}

static inline int SignExtend11(int v) {
    // on real hardware, VRAM offsets do not exactly wrap at 1024. Instead,
    // coords are treated as a signed 11-bit value. Meaning 1025 will become
    // -1023, and not 1. This fixes games like FF7 using a negative draw ofs.
    int x = v & 0x7FF;
    return x & 0x400 ? x - 0x800 : x;
}

static int trace_scene = -1;
static int trace_timer = -1;

static unsigned int HashTracePixels(
    const unsigned short* pixels, size_t count) {
    unsigned int hash = 2166136261u;
    size_t i;
    for (i = 0; i < count; i++) {
        hash ^= pixels[i] & 0xff;
        hash *= 16777619u;
        hash ^= pixels[i] >> 8;
        hash *= 16777619u;
    }
    return hash;
}

static void TraceVramLoad(const RECT* rect, const u_long* pixels) {
    size_t count;
    size_t nonzero = 0;
    size_t i;
    if (!getenv("RAGE_GPU_VRAM_TRANSFER_TRACE"))
        return;
    count = (size_t)(unsigned short)rect->w * (unsigned short)rect->h;
    for (i = 0; i < count; i++) {
        if (((const unsigned short*)pixels)[i] != 0)
            nonzero++;
    }
    fprintf(stderr,
            "vram-transfer kind=load scene=%d timer=%d rect=%d,%d,%d,%d "
            "pixels=%zu nonzero=%zu hash=%08x\n",
            trace_scene, trace_timer, rect->x, rect->y, rect->w, rect->h, count,
            nonzero, HashTracePixels((const unsigned short*)pixels, count));
}

static void TraceVramTransfer(
    const char* kind, const RECT* rect, int dstX, int dstY) {
    if (!getenv("RAGE_GPU_VRAM_TRANSFER_TRACE"))
        return;
    fprintf(stderr,
            "vram-transfer kind=%s scene=%d timer=%d rect=%d,%d,%d,%d "
            "dst=%d,%d\n",
            kind, trace_scene, trace_timer, rect->x, rect->y, rect->w, rect->h,
            dstX, dstY);
}

void Psyz_GpuTraceContext(int scene, int timer) {
    trace_scene = scene;
    trace_timer = timer;
}

static void TraceOtNode(
    unsigned long long chain_index, unsigned node_index, const DR_ENV* packet) {
    static int initialized;
    static int enabled;
    static int has_scene;
    static int has_timer;
    static int wanted_scene;
    static int wanted_timer;
    if (!initialized) {
        const char* scene_text = getenv("RAGE_GPU_GP0_TRACE_SCENE");
        const char* timer_text = getenv("RAGE_GPU_GP0_TRACE_TIMER");
        enabled = getenv("RAGE_GPU_OT_TRACE") != NULL;
        has_scene = scene_text != NULL;
        has_timer = timer_text != NULL;
        if (scene_text)
            wanted_scene = (int)strtol(scene_text, NULL, 0);
        if (timer_text)
            wanted_timer = (int)strtol(timer_text, NULL, 0);
        initialized = 1;
    }
    if (!enabled || (has_scene && trace_scene != wanted_scene) ||
        (has_timer && trace_timer != wanted_timer))
        return;
    if (isendprim(packet)) {
        fprintf(stderr,
                "gp0-node chain=%llu node=%u address=%p next=end words=%u\n",
                chain_index, node_index, (const void*)packet, getlen(packet));
    } else {
        fprintf(stderr,
                "gp0-node chain=%llu node=%u address=%p next=%p words=%u\n",
                chain_index, node_index, (const void*)packet,
                (void*)nextPrim(packet), getlen(packet));
    }
}

/*
 * Host ordering-table links are pointer-sized, but the GPU command stream is
 * always a dense sequence of 32-bit GP0 words.  Primitive structs place their
 * packed payload immediately after O_TAG; drawing-environment packets retain
 * u_long command slots for source compatibility.  Normalize both layouts at
 * this boundary so every renderer and diagnostic consumes the same format.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
/*
 * Big-endian hosts (Saturn SH-2): primitive structs are laid out field by
 * field for the PS1, so a 32-bit load of payload word i returns its bytes in
 * the wrong order. Rebuild the value a PS1 load returns, by word kind from the
 * GP0 command format (code has bits 0-1 cleared):
 *   colour/code (4 u_char): reverse the bytes
 *   vertex, w/h, polyline terminator (2 short): swap the halves
 *   uv + clut/tpage/pad (2 u_char + u_short): swap halves, swap low bytes
 */
static u32 PsyzBeGp0Word(int code, int i, u32 w) {
    int colour = i == 0;
    int uv = 0;
    int j;

    if (i > 0 && code < 0x40) {
        /* polygon: vertex 0 = xy[,uv]; later vertices = [rgb] xy [uv] */
        int tex = (code & 0x04) != 0;
        int per = 1 + ((code & 0x10) != 0) + tex;

        j = i - 1;
        if (j >= 1 + tex) {
            j = (j - 1 - tex) % per;
            if (code & 0x10) {
                colour = j == 0;
                j--;
            }
        }
        uv = j == 1;
    } else if (i > 0 && code < 0x60) {
        /* line: gouraud = xy, then rgb,xy,...; flat = all xy */
        colour = (code & 0x10) && i >= 2 && i % 2 == 0;
    } else if (i > 0) {
        /* rect: xy, [uv if textured], [w/h if variable size] */
        uv = i == 2 && (code & 0x04);
    }
    if (colour)
        return w >> 24 | (w >> 8 & 0xFF00u) | (w << 8 & 0xFF0000u) | w << 24;
    w = w >> 16 | w << 16;
    if (uv)
        w = (w & 0xFFFF0000u) | (w >> 8 & 0xFFu) | (w << 8 & 0xFF00u);
    return w;
}
#endif

static int CanonicalizePacket(
    const DR_ENV* packet, u_long* output, int output_words) {
    int length = getlen(packet);
    int code;
    int i;

    if (length < 0 || length > output_words)
        return -1;
    if (length == 0)
        return 0;

    code = getcode(packet) & ~3;
    if (code >= 0x20 && code < 0x80) {
        const u32* packed = (const u32*)packet->code;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        for (i = 0; i < length; i++)
            output[i] = PsyzBeGp0Word(code, i, packed[i]);
#else
        for (i = 0; i < length; i++)
            output[i] = packed[i];
#endif
    } else {
        for (i = 0; i < length; i++)
            output[i] = packet->code[i] & 0xFFFFFFFFu;
    }
    return length;
}

static void TraceCanonicalPacket(
    unsigned long long chain_index, unsigned packet_index, unsigned code,
    const DR_ENV* packet, const u_long* words, int length) {
    static int initialized;
    static int enabled;
    static int has_frame;
    static int has_scene;
    static int has_timer;
    static unsigned long long wanted_frame;
    static int wanted_scene;
    static int wanted_timer;
    PsyzVideoStats stats;
    int i;

    if (!initialized) {
        const char* frame_text = getenv("RAGE_GPU_GP0_TRACE_FRAME");
        const char* scene_text = getenv("RAGE_GPU_GP0_TRACE_SCENE");
        const char* timer_text = getenv("RAGE_GPU_GP0_TRACE_TIMER");
        enabled = getenv("RAGE_GPU_GP0_TRACE") != NULL;
        has_frame = frame_text != NULL;
        has_scene = scene_text != NULL;
        has_timer = timer_text != NULL;
        if (frame_text)
            wanted_frame = strtoull(frame_text, NULL, 0);
        if (scene_text)
            wanted_scene = (int)strtol(scene_text, NULL, 0);
        if (timer_text)
            wanted_timer = (int)strtol(timer_text, NULL, 0);
        initialized = 1;
    }
    if (!enabled || length == 0)
        return;
    if (Psyz_VideoStats(&stats) != 0)
        stats.total_frames = 0;
    if (has_frame && stats.total_frames != wanted_frame)
        return;
    if (has_scene && trace_scene != wanted_scene)
        return;
    if (has_timer && trace_timer != wanted_timer)
        return;

    fprintf(stderr,
            "gp0-packet frame=%llu scene=%d timer=%d chain=%llu packet=%u "
            "address=%p code=%02x length=%d "
            "words=",
            stats.total_frames, trace_scene, trace_timer, chain_index,
            packet_index, (const void*)packet, code, length);
    for (i = 0; i < length; i++) {
        fprintf(stderr, "%s%08x", i ? "," : "", (unsigned)words[i]);
    }
    fputc('\n', stderr);
}

static void DispatchPackets(u_long* buf, int len) {
    RECT rect;
    unsigned int x, y;
    for (int i = 0; i < len; i++) {
        u_long op = buf[i];
        int code = (int)(buf[i] >> 24) & 0xFF;
        // https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gpu-render-polygon-commands
        switch (code) {
        case 0xFF:
            /* Native OT links are pointer-sized.  The flattened 64-bit walk
             * can retain the terminal link word after the last packet; it is
             * a DMA-chain terminator, not a GP0 command. */
            if (i == len - 1)
                break;
            WARNF("unsupported command %02X", code);
            break;
        case 0x00:
            // empty?!
            break;
        case 0x01: // clear cache
            GPU_clear_cache();
            break;
        case 0x02: // frame buffer rectangle draw
            rect.x = (short)(buf[i + 1] & 0xFFFF);
            rect.y = (short)((buf[i + 1] >> 16) & 0xFFFF);
            rect.w = (short)(buf[i + 2] & 0xFFFF);
            rect.h = (short)((buf[i + 2] >> 16) & 0xFFFF);
            Draw_ClearImage(
                &rect, (u_char)(op & 0xFF), (u_char)((op >> 8) & 0xFF),
                (u_char)((op >> 16) & 0xFF));
            i += 2;
            break;
        case 0x80: // move image
            rect.x = (short)(buf[i + 1] & 0xFFFF);
            rect.y = (short)((buf[i + 1] >> 16) & 0xFFFF);
            rect.w = (short)(buf[i + 3] & 0xFFFF);
            rect.h = (short)((buf[i + 3] >> 16) & 0xFFFF);
            x = (short)(buf[i + 2] & 0xFFFF);
            y = (short)((buf[i + 2] >> 16) & 0xFFFF);
            TraceVramTransfer("move", &rect, x, y);
            Draw_MoveImage(&rect, x, y);
            i += 3;
            break;
        case 0xA0: // write image
            GPU_write_image();
            break;
        case 0xC0: // read image
            GPU_read_image();
            break;
        case 0xE1:
            GPU_STATUS = (GPU_STATUS & ~0x7FF) | (u32)(op & 0x7FF);
            Draw_SetTexpageMode((ParamDrawTexpageMode*)&op);
            break;
        case 0xE2:
            // https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp0e2h-texture-window-setting
            Draw_SetTextureWindow(op & 0x1F, (op >> 5) & 0x1F,
                                  (op >> 10) & 0x1F, (op >> 15) & 0x1F);
            break;
        case 0xE3:
            Draw_SetAreaStart((int)op & 0x3FF, (int)(op >> 10) & 0x3FF);
            break;
        case 0xE4:
            Draw_SetAreaEnd((int)op & 0x3FF, (int)(op >> 10) & 0x3FF);
            break;
        case 0xE5:
            Draw_SetOffset(
                SignExtend11((int)op), SignExtend11((int)(op >> 11)));
            break;
        case 0xE6:
            Draw_SetMask(!!(op & 1), !!(op & 2));
            break;
        default:
            if (code >= 0x20 && code < 0x80) {
                i += Draw_PushPrim(&buf[i], len - i) - 1;
                break;
            }
            if (user_gpu_commands[code].handler) {
                int consumed = user_gpu_commands[code].handler(
                    &buf[i], len - i, user_gpu_commands[code].userdata);
                if (consumed > 0 && consumed <= len - i) {
                    i += consumed - 1;
                    break;
                }
                WARNF("custom command %02X returned invalid length %d", code,
                      consumed);
                break;
            }
            WARNF("unsupported command %02X", code);
        }
    }
}

static void GPU_Flush(void) {
    if (sizeof(void*) == 4) {
        // 32-bit targets dispatch straight into the draw buffer
        Draw_FlushBuffer();
    }
    Draw_ResetBuffer();
    if (queue_len > 0) {
        DispatchPackets(queue_buf, queue_len);
        Draw_FlushBuffer();
    }
    queue_len = 0;
}

// A PS1 transfer is only late when queued behind GPU work: StoreImage writes
// its destination at DrawSync while drawing is pending since the last sync.
typedef struct {
    const u8* p;
    size_t bytes;
} PendingStore;

static PendingStore pending_stores[64];
static int n_pending_stores;
static int gpu_busy;

static size_t TransferBytes(const RECT* rect) {
    // the DMA moves whole words
    return (((size_t)rect->w * (size_t)rect->h + 1) & ~(size_t)1) * 2;
}

static void GPU_Sync(void) {
    Draw_ExequeSync();
    n_pending_stores = 0;
}

static int IsPendingStoreDst(const RECT* rect, const u_long* data) {
    const u8* p = (const u8*)data;
    size_t bytes = TransferBytes(rect);
    for (int i = 0; i < n_pending_stores; i++) {
        const PendingStore* s = &pending_stores[i];
        if (p < s->p + s->bytes && s->p < p + bytes) {
            return 1;
        }
    }
    return 0;
}

int Psyz_GpuExeque() {
    GPU_Flush();
    GPU_Sync();
    gpu_busy = 0;
    return 0;
}

static void GPU_SyncIfPending(void) {
    if (n_pending_stores) {
        Psyz_GpuExeque();
    }
}

int Psyz_GpuReplayBegin(void) {
    Psyz_GpuExeque();
    Draw_ResetBuffer();
    return 0;
}

int Psyz_GpuReplayPacket(const unsigned int* words, int count) {
    u_long* native_words;
    int i;
    if (words == NULL || count <= 0)
        return -1;
    native_words = malloc((size_t)count * sizeof(*native_words));
    if (native_words == NULL)
        return -1;
    for (i = 0; i < count; i++)
        native_words[i] = words[i];
    DispatchPackets(native_words, count);
    free(native_words);
    return 0;
}

int Psyz_GpuReplayEnd(void) {
    Draw_FlushBuffer();
    Draw_ExequeSync();
    return 0;
}

static void GPU_Enqueue(u_long* packets) {
    static unsigned long long chain_index;
    unsigned packet_index = 0;
    unsigned long long current_chain = chain_index++;
    DR_ENV* env = (DR_ENV*)packets;
    gpu_busy = 1;
#if !(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    if (sizeof(u_long) == 4) {
        // fast path for 32-bit little-endian systems
        while (1) {
            if (env->len > 0) {
                DispatchPackets((u_long*)env->code, (int)env->len);
            }
            if (isendprim(env)) {
                break;
            }
            env = (DR_ENV*)nextPrim(env);
        }
#ifdef __EMSCRIPTEN__
        Draw_FlushBuffer();
#endif
        return;
    }
#endif
    while (1) {
        int env_len = getlen(env);
        TraceOtNode(current_chain, packet_index, env);
        if (env_len > 0x100) {
            ERRORF("packet %p is 0x%X words long, likely corrupted", (void*)env,
                   env_len);
            break;
        }
        if (queue_len + env_len > LEN(queue_buf)) {
            GPU_Flush();
        }
        {
            int canonical_len = CanonicalizePacket(
                env, queue_buf + queue_len, LEN(queue_buf) - queue_len);
            if (canonical_len < 0) {
                ERRORF("cannot canonicalize packet %p code=%02X len=%u",
                       (void*)env, getcode(env), getlen(env));
                break;
            }
            TraceCanonicalPacket(current_chain, packet_index++, getcode(env),
                                 env, queue_buf + queue_len, canonical_len);
        }
        queue_len += env_len;
        if (isendprim(env)) {
            break;
        }
        {
            DR_ENV* next = (DR_ENV*)nextPrim(env);
            if (((uintptr_t)next & (sizeof(u32) - 1)) != 0) {
                ERRORF("misaligned OT link %p after packet %p code=%02X len=%u",
                       (void*)next, (void*)env, getcode(env), getlen(env));
                break;
            }
            env = next;
        }
    }
}

// Weird fix due to addque2 calling the callback with two u_long params
static int GPU_EnqueueTrampoline(u_long p1, u_long p2) {
    GPU_Enqueue((u_long*)p1);
    return 0;
}

static int GPU_DataWrite(RECT* rect, u_long* data) {
    GPU_Flush();
    if (IsPendingStoreDst(rect, data)) {
        GPU_Sync();
    }
    TraceVramLoad(rect, data);
    Draw_LoadImage(rect, data);
    return 0;
}
static int GPU_DataRead(RECT* rect, u_long* data) {
    GPU_Flush();
    if (n_pending_stores == LEN(pending_stores)) {
        GPU_Sync();
    }
    TraceVramTransfer("store", rect, -1, -1);
    Draw_StoreImage(rect, data);
    pending_stores[n_pending_stores].p = (const u8*)data;
    pending_stores[n_pending_stores].bytes = TransferBytes(rect);
    n_pending_stores++;
    if (!gpu_busy) {
        GPU_Sync();
    }
    return 0;
}
static int psyz_cwb(u32* data, s32 n) {
    gpu_busy = 1;
    if (n > 0) {
        Draw_PushPrim((u_long*)data, n);
    }
    return 0;
}

static int _param(int x) { return 0; }
static int psyz_addque2(
    int (*exec)(u_long p1, u_long p2), u_long p1, int len, u_long p2) {
    return exec(p1, p2);
}
static int psyz_addque(
    int (*exec)(u_long p1, u_long p2), u_long p1, u_long p2) {
    return psyz_addque2(exec, p1, 0, p2);
}

// psyz_clr is very similar to _clr from libgpu/sys.c
static DR_ENV clear_cmd;
static int psyz_clr(RECT* rect, u32 color) {
    rect->w = CLAMP(rect->w, 0, VRAM_W - 1);
    rect->h = CLAMP(rect->h, 0, VRAM_H - 1);

#if 1 // HACK: faster but more inaccurate path, doesn't need a Psyz_GpuExeque
    setlen(&clear_cmd, 4);
    clear_cmd.code[0] = 0xE6000000; // mask bit setting
    clear_cmd.code[1] = (color & 0xFFFFFF) | 0x02000000;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // the u32 pun reads y,x on BE; pack the PS1 word (x low)
    clear_cmd.code[2] = (u32)(u16)rect->y << 16 | (u16)rect->x;
    clear_cmd.code[3] = (u32)(u16)rect->h << 16 | (u16)rect->w;
#else
    clear_cmd.code[2] = (u_long) * (u32*)&rect->x;
    clear_cmd.code[3] = (u_long) * (u32*)&rect->w;
#endif
#else
    // Needs a sync so 0xE1 CMDs can update GPU_STATUS before it gets used.
    Psyz_GpuExeque();
    setlen(&clear_cmd, 5);
    clear_cmd.code[0] = 0xE6000000; // mask bit setting
    clear_cmd.code[1] = 0xE1000000 | GPU_STATUS & 0x7FF | (color >> 0x1F) << 10;
    clear_cmd.code[2] = (color & 0xFFFFFF) | 0x02000000;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    clear_cmd.code[3] = (u32)(u16)rect->y << 16 | (u16)rect->x;
    clear_cmd.code[4] = (u32)(u16)rect->h << 16 | (u16)rect->w;
#else
    clear_cmd.code[3] = (u_long) * (u32*)&rect->x;
    clear_cmd.code[4] = (u_long) * (u32*)&rect->w;
#endif
#endif

    termPrim(&clear_cmd);
    GPU_Enqueue((u_long*)&clear_cmd);
    return 0;
}

void Psyz_GpuDisplayCommand(unsigned int cmd) {
    unsigned char op = (cmd >> 24) & 0x3F;
    switch (op) {
    case 0:
        Draw_Reset();
        break;
    case 1:
        LOG_ONCE("Reset FIFO not implemented");
        break;
    case 2:
        LOG_ONCE("Ack IRQ not implemented");
        break;
    case 3:
        Draw_DisplayEnable(!(cmd & 1));
        break;
    case 4:
        LOG_ONCE("DMA direction not implemented");
        break;
    case 5:
        Draw_DisplayArea(cmd & 0x3FE, (cmd >> 10) & 0x3FF);
        break;
    case 6:
        Draw_DisplayHorizontalRange(cmd & 0xFFF, (cmd >> 12) & 0xFFF);
        break;
    case 7:
        Draw_DisplayVerticalRange(cmd & 0x3FF, (cmd >> 10) & 0x3FF);
        break;
    case 8:
        cmd &= 0xFFFFFF;
        Draw_SetDisplayMode((DisplayMode*)&cmd);
        break;
    default:
        WARNF("unhandled ctl %02X (%08X)", op, cmd);
        break;
    }
}
static int psyz_getctl(int _) {
    NOT_IMPLEMENTED;
    return 0;
}

static int psyz_otc(OT_TYPE* ot, s32 n) {
    s32 i;

    if (n <= 0) {
        return 0;
    }
    for (i = n - 1; i > 0; i--) {
        setaddr(&ot[i], &ot[i - 1]);
        setlen(&ot[i], 0);
    }
    setaddr(&ot[0], 0xFFFFFF);
    setlen(&ot[0], 0);
    return 0;
}

static int psyz_param(int _) {
    NOT_IMPLEMENTED;
    return 0;
}
static int psyz_reset(int _) {
    GPU_SyncIfPending();
    return GPU_V0;
}
static u_long psyz_status(void) {
    NOT_IMPLEMENTED;
    return 0;
}
static int psyz_sync(int mode) {
    // see decomp/src/libgpu/sys.c
    // mode 0 waits until all the queue is drawn on screen
    // mode 1 process the queue and return how many elements have been queued
    // return -1 if GPU has timed out
    // PsyZ completes everything in both modes
    Psyz_GpuExeque();
    return 0;
}

int psyz_gpu_version(int mode) {
    GPU_SyncIfPending();
    return GPU_V0;
}

// forwards raw GP0 words into the internal queue, then trigger execution.
void Psyz_GpuWriteGP0(unsigned int word) {
    gpu_busy = 1;
    if (queue_len >= (int)LEN(queue_buf)) {
        WARNF("GPU queue full");
        return;
    }
    queue_buf[queue_len++] = (u_long)word;
}

void GPU_cw(u_long* param) {
    GpuVtable* gpu = (GpuVtable*)param;
    gpu->ver = "psyz";
    gpu->addque = psyz_addque;
    gpu->addque2 = psyz_addque2;
    gpu->clr = psyz_clr;
    gpu->ctl = Psyz_GpuDisplayCommand;
    gpu->cwb = psyz_cwb;
    gpu->cwc = (void (*)(u_long*))GPU_EnqueueTrampoline;
    gpu->drs = GPU_DataRead;
    gpu->dws = GPU_DataWrite;
    gpu->exeque = Psyz_GpuExeque;
    gpu->getctl = psyz_getctl;
    gpu->otc = psyz_otc;
    gpu->param = psyz_param;
    gpu->reset = psyz_reset;
    gpu->status = psyz_status;
    gpu->sync = psyz_sync;
}

// these are not yet decompiled
int _addque2(int (*exec)(u_long p1, u_long p2), u_long p1, int len, u_long p2) {
    return 0;
}
int _exeque() { return 0; }
int get_alarm(void) { return 0; }
