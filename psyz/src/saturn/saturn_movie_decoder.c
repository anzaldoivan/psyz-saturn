/* saturn_movie_decoder.c -- Cinepak FILM decoder vendored from libyaul_cinepak
 * c186f56 decoder.c + decoder.h (MIT, text below). The same file builds
 * unpatched with x86 gcc and with sh2 gcc.
 *
 * Deltas from upstream, each with its reason:
 * - CD: cd.h, CD_BLOCK_DATA_2, queueDiskRead, the 8-sector prefetch and both
 *   waits (decoder.c:229-271, unbounded CD_STATUS_WAIT :255-262) removed;
 *   stream_readbytes/skip call movie_stream_refill (the service's HWRAM ring or
 *   the host harness), so host and target share one stream layer.
 * - Endianness: stream_read16/32, the STAB fields, numStrips, the strip header
 *   words and the 4 flag loads assemble big-endian bytes explicitly, pixels
 *   use explicit 1555 shifts (were native SH-2 loads and the BE rgb1555_t
 *   bitfields), so x86 needs no host patch.
 * - Display: copyVideoFrame's vdp_dma_enqueue (:146) and its unbounded
 *   vdp_dma_count_get wait (:136) -> movie_frame_out(frame, k); its nonzero
 *   return is a status. initialize_film (VDP2 clear and sync) removed: the
 *   service owns the display.
 * - DMAC: stripdata_copyLastCodebooks, dmaCopyBlocksDone, cpu_cache_purge and
 *   the copy waits (:98-130) removed; the memcpy twin (_nodma, the one the
 *   parser calls) kept: no DMA, nothing to wait for.
 * - FRT: frtOviHandler, frtTimerStart/Ellapsed (:49-60), cpu_frt_ovi_set, the
 *   per-second statistics and the ms busy-wait (:970-1046) removed: pacing is
 *   the service's VBlank schedule, fed by movie_tick at every strip and chunk
 *   boundary.
 * - Sound: film_audio_* callbacks and pcmsys.h -> movie_audio_chunk(n), which
 *   consumes the next n stream bytes as PCM (no staging buffer); the
 *   stereo-as-mono skip kept; movie_audio_table(n, channels) after the
 *   sample table lets the service write the sound ahead of the decoder.
 * - Guards: strip length < 12 -> MOVIE_STRIP_SHORT, raw chunk
 *   length < 4 -> MOVIE_CHUNK_SHORT (:814, :823), each a single line tagged
 *   guard:strip or guard:chunk;
 *   readVectors' tmpBuffer read (:524, wraps when the chunk is short) longer
 *   than TMP_BUFFER_SIZE -> MOVIE_CHUNK_SHORT.
 * - Header: the compiled-out DEBUG_REQUIREs on FILM/FDSC/cvid/STAB magic and
 *   version 1.09 (:900-946), the sample cache overflow that only logged
 *   (:180-186; logError spins forever without debug) and a file shorter than
 *   its header -> MOVIE_BAD_HEADER before the first sample.
 * - Stop: film_loop_handler (kept per sample, upstream API) or a seam
 *   returning nonzero -> MOVIE_STOPPED. After any status the stream reads
 *   nothing more and the sample's parse returns at the next strip or chunk.
 * - Entry: play_film(cdfs entry, cache, n), void -> movie_decoder_play(
 *   file_size, cache, n) returning the status.
 * - Logging: logMessage/logError/clearLog (dbgio), DEBUG_REQUIRE*, the
 *   DEBUG_DECODER dumps and the unknown-chunk sprintf into LWRAM removed, with
 *   base.h, intellisense.h, yaul.h and gamemath.h: no platform header left.
 * - Linkage: every file-scope symbol static except the entry (no clash with
 *   title symbols such as tmpBuffer; inline needs no -fgnu89-inline); the
 *   decoder.h types the seams do not use moved here.
 * - Frame buffers: two (videoTmpBuffer[2]), alternated per frame. Frame k's
 *   DMA runs at the next VBlank-in, inside libyaul's VBlank-in handler, while
 *   frame k+1 already decodes; one buffer would let k+1's pixels reach VRAM
 *   as part of frame k. Inter-coded strips skip blocks that keep the previous
 *   frame's pixels; the buffer now written holds frame k-1, so readVectorsInter
 *   copies each skipped 4x4 block from the other buffer (carryBlock), and so
 *   does parseVideo for a strip's blocks past its last coded one. Per
 *   block, not a whole-frame memcpy at each switch: 32 B per skipped block
 *   instead of 153,600 B per frame on the SH-2, same pixels. Both
 *   buffers are static .bss, in cached WORK RAM-H (the title's .bss starts at
 *   0x06012C40; the first buffer sat at 0x0601CE54, 0x25800 B). The VRAM copy
 *   is libyaul's SCU DMA (vdp_sync.c _dma_queue_transfer, level 1, after
 *   cpu_cache_purge, so the cached pixel writes reach WORK RAM-H first). The
 *   second buffer adds 153,600 B: the title's .bss end moves from 0x0609EB40
 *   to 0x060C4340, inside WORK RAM-H's 1 MB (0x06100000).
 *   Each play starts in buffer 0: after a play with an odd frame count the
 *   next play's frame 0 would land in buffer 1 and its frame 1 would carry
 *   from the buffer it writes.
 * - Codebook updates (0x2100/0x2300): one read of the whole chunk, then flags
 *   and 6-byte entries parsed from it, bounded by the chunk length and by the
 *   256-entry book; upstream made one stream read per flag word and per entry,
 *   each a movie_stream_refill (a ring copy on the Saturn).
 * - Views: the vector chunks (0x3000-0x3200) and the codebook chunks read
 *   their bytes through stream_view, in place in the service's ring when
 *   movie_stream_view can hand them out contiguously, else copied into
 *   tmpBuffer as before. readVectorsInter: chunk length < 4 ->
 *   MOVIE_CHUNK_SHORT (upstream read flags past the chunk).
 * - Codebooks: the YUV books (codebook_t v1/v4, read only by the conversion)
 *   are gone; entries convert to RGB1555 straight from the chunk bytes, at
 *   most 256 per book (upstream wrote past the book). A strip that inherits
 *   the previous strip's books copies each book (2048 B) only when a chunk
 *   first reads or patches it, or at the strip's end; a full book of 256
 *   entries replaces it without the copy. Same books, so the same pixels.
 * Unchanged: the Cinepak parse and render (:368-800), the FILM header walk,
 * the STAB sample cache, the 320x240 frame size.
 *
 * Copyright (c) 2022-2025 Romulo Fernandes
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
#include "saturn_movie_decoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif

#ifndef CLAMP
#define CLAMP(V, X, Y) ((V) < (X) ? (X) : ((V) > (Y) ? (Y) : (V)))
#endif

// A sample stored in the STAB table is: offset, length, info1, info2 (BE u32).
#define CD_FILM_SAMPLE_SIZE 16

typedef struct {
    film_sample_t* samples;
    uint32_t numSamples;
    uint32_t currentSample;
} film_sample_cache_t;

typedef struct {
    uint32_t size;
    uint32_t offset;

    film_sample_cache_t sampleCache;
} binary_stream_t;

#define MAX_STRIPS 16

typedef struct {
    uint16_t color[4];
} codebook555_t;

typedef struct {
    codebook555_t v1RGB[256];
    codebook555_t v4RGB[256];
} __attribute__((aligned(16))) strip_codebook_t;

typedef struct {
    strip_codebook_t codebooks[MAX_STRIPS];
    uint32_t strip;
    uint32_t pending; /* CB_V1 | CB_V4: books still to copy from strip - 1 */

    uint32_t topX;
    uint32_t writeX;
    uint32_t bottomX;

    uint32_t topY;
    uint32_t writeY;
    uint32_t bottomY;

} stripdata_t;

#define CB_V1 1
#define CB_V4 2

// Globals
static stripdata_t stripData;
uint32_t movie_decoder_cbcopies;

/* MOVIE_* status of the running play; nonzero stops the stream. */
static int film_status = MOVIE_END;

/* One-line guard: set the status and leave the caller. */
#define MOVIE_GUARD(cond, st)                                                  \
    do {                                                                       \
        if (cond) {                                                            \
            film_status = (st);                                                \
            return;                                                            \
        }                                                                      \
    } while (0)

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
static uint32_t videoWidth = VIDEO_WIDTH;
static uint32_t videoHeight = VIDEO_HEIGHT;
static uint32_t videoStartY = 0;
static uint16_t videoTmpBuffer[2][VIDEO_WIDTH * VIDEO_HEIGHT];
static uint16_t* vdp2ImagePtr = videoTmpBuffer[0];
static const uint16_t* prevImagePtr = videoTmpBuffer[1];

// Audio data
static uint8_t audioChannels = 0;
static uint8_t audioSamplingResolution = 0;
static uint8_t audioCompression = 0;
static uint16_t audioSamplingFrequencyHz = 0;
static uint32_t audioNumPlayedSamples = 0;

// For saving reads
#define TMP_BUFFER_SIZE 0x10000
static uint8_t tmpBuffer[TMP_BUFFER_SIZE];

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

/* The one stream read: dst NULL skips. Once a status is set nothing is read
 * (dst untouched) but the offset still advances, so every loop stays bounded.
 */
static void stream_readbytes(binary_stream_t* stream, void* dst, uint32_t len) {
    if (film_status == MOVIE_END && movie_stream_refill(dst, len) != 0)
        film_status = MOVIE_STOPPED;

    stream->offset += len;
}

static void stream_skip(binary_stream_t* stream, uint32_t len) {
    stream_readbytes(stream, NULL, len);
}

static uint16_t stream_read16(binary_stream_t* stream) {
    uint8_t data[2] = {0};
    stream_readbytes(stream, data, 2);

    return be16(data);
}

static uint32_t stream_read32(binary_stream_t* stream) {
    uint8_t data[4] = {0};
    stream_readbytes(stream, data, 4);

    return be32(data);
}

/* The next len bytes (len <= TMP_BUFFER_SIZE), in place when the stream can
 * view them, else read into tmpBuffer. Valid until the next stream call. */
static const uint8_t* stream_view(binary_stream_t* stream, uint32_t len) {
    const uint8_t* p = film_status == MOVIE_END ? movie_stream_view(len) : NULL;

    if (!p) {
        stream_readbytes(stream, tmpBuffer, len);
        return tmpBuffer;
    }
    stream->offset += len;
    return p;
}

static inline uint32_t stream_pos(binary_stream_t* stream) {
    return stream->offset;
}

/* book: one 6-byte entry, y[4] (u8), u, v (s8). */
static inline void codebook555_new(const uint8_t* book, codebook555_t* cb555) {
    // | r |   | 1.0  0.0  2.0 | | y |
    // | g | = | 1.0 -0.5 -1.0 | | u |
    // | b |   | 1.0  2.0  0.0 | | v |
    const int8_t u = (int8_t)book[4];
    const int8_t v = (int8_t)book[5];
    const int16_t cr = (v << 1);
    const int16_t cg = -(u >> 1) - v;
    const int16_t cb = +(u << 1);

    for (uint32_t i = 0; i < 4; ++i) {
        const int16_t r = book[i] + cr;
        const int16_t g = book[i] + cg;
        const int16_t b = book[i] + cb;

        const uint8_t nr = CLAMP(r, 0, 255);
        const uint8_t ng = CLAMP(g, 0, 255);
        const uint8_t nb = CLAMP(b, 0, 255);

        // RGB1555: msb 1 (bit 15), b 14-10, g 9-5, r 4-0
        cb555->color[i] =
            (uint16_t)(0x8000 | (nb >> 3) << 10 | (ng >> 3) << 5 | (nr >> 3));
    }
}

static void stripdata_new(stripdata_t* data) {
    memset(data->codebooks, 0, MAX_STRIPS * sizeof(strip_codebook_t));
    data->strip = 0;
    data->pending = 0;
    data->writeX = data->topX = 0;
    data->bottomX = 320;
    data->writeY = data->topY = 0;
    data->bottomY = 240;
}

/* Copies the pending books among `books` from the previous strip. */
static void stripdata_copyLastCodebooks_nodma(
    stripdata_t* data, uint32_t books) {
    strip_codebook_t* cb = &data->codebooks[data->strip];

    books &= data->pending;
    if (books & CB_V1) {
        memcpy(cb->v1RGB, cb[-1].v1RGB, sizeof(cb->v1RGB));
        movie_decoder_cbcopies++;
    }
    if (books & CB_V4) {
        memcpy(cb->v4RGB, cb[-1].v4RGB, sizeof(cb->v4RGB));
        movie_decoder_cbcopies++;
    }
    data->pending &= ~books;
}

static inline codebook555_t* stripdata_getV1Codebook555(stripdata_t* data) {
    return data->codebooks[data->strip].v1RGB;
}

static inline codebook555_t* stripdata_getV4Codebook555(stripdata_t* data) {
    return data->codebooks[data->strip].v4RGB;
}

// stream must be exactly at the start of the sample descriptions
static void film_sample_cache_new(
    binary_stream_t* stream, uint32_t totalNumSamples) {
    film_sample_t* outputSamples = stream->sampleCache.samples;
    for (volatile uint32_t i = 0; i < totalNumSamples; ++i) {
        uint8_t sample[CD_FILM_SAMPLE_SIZE] = {0};
        stream_readbytes(stream, sample, CD_FILM_SAMPLE_SIZE);

        const bool isAudio = (be32(sample + 8) == 0xFFFFFFFF);

        film_sample_t* newSample = &outputSamples[i];
        if (isAudio) {
            newSample->interval = 0xFFFFFFFF;
        } else {
            newSample->interval = be32(sample + 12);
        }

        newSample->length = be32(sample + 4);
    }
}

// Return the current sample, don't do anything else.
static inline film_sample_t film_sample_get_next_sample(
    film_sample_cache_t* cache) {
    return cache->samples[cache->currentSample++];
}

static void stream_new(binary_stream_t* stream, uint32_t size,
                       film_sample_t* sampleCache, uint32_t sampleCacheSize) {
    stream->size = size;
    stream->offset = 0;

    stream->sampleCache.samples = sampleCache;
    stream->sampleCache.numSamples = sampleCacheSize;
    stream->sampleCache.currentSample = 0;
}

static inline bool film_sample_is_video(const film_sample_t* sample) {
    return sample->interval != 0xFFFFFFFF;
}

static inline bool film_sample_is_audio(const film_sample_t* sample) {
    return sample->interval == 0xFFFFFFFF;
}

static inline void stripdata_skipBlock(stripdata_t* data) {
    data->writeX += 4;
    if (data->writeX >= data->bottomX) {
        data->writeX = data->topX;
        data->writeY += 4;
    }
}

/* An inter strip's skipped block keeps the previous frame's pixels: copy
 * them from the other buffer (rows clipped at bottomY as renderPixel1). Plain
 * stores, no memcpy: a single buffer makes this a harmless self-copy. */
static void carryBlock(const stripdata_t* data) {
    const uint32_t y = data->writeY;
    uint32_t imageIndex = ((videoStartY + y) * VIDEO_WIDTH) + data->writeX;

    for (uint32_t row = 0; row < 4 && y + row < data->bottomY; ++row) {
        vdp2ImagePtr[imageIndex] = prevImagePtr[imageIndex];
        vdp2ImagePtr[imageIndex + 1] = prevImagePtr[imageIndex + 1];
        vdp2ImagePtr[imageIndex + 2] = prevImagePtr[imageIndex + 2];
        vdp2ImagePtr[imageIndex + 3] = prevImagePtr[imageIndex + 3];
        imageIndex += VIDEO_WIDTH;
    }
}

static void renderPixel1(stripdata_t* data, uint8_t c0) {
    const codebook555_t e0 = stripdata_getV1Codebook555(data)[c0];
    const uint32_t x = data->writeX;
    const uint32_t y = data->writeY;

    // VDP2 image has 512x256
    uint32_t imageIndex = ((videoStartY + y) * VIDEO_WIDTH) + x;

    // +----+----+  +---+  +---+
    // | y0 | y1 |  | u |  | v |
    // +----+----+  +---+  +---+
    // | y2 | y3 |
    // +----+----+

    if (y + 0 >= data->bottomY)
        return;

    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[0];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[1];

    if (y + 1 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH;
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[0];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[1];

    if (y + 2 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH;
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[2];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[3];

    if (y + 3 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH;
    vdp2ImagePtr[imageIndex] = vdp2ImagePtr[imageIndex + 1] = e0.color[2];
    vdp2ImagePtr[imageIndex + 2] = vdp2ImagePtr[imageIndex + 3] = e0.color[3];
}

static void renderPixel4(
    stripdata_t* data, uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {

    const codebook555_t* codebook = stripdata_getV4Codebook555(data);
    const codebook555_t* e0 = &codebook[c0];
    const codebook555_t* e1 = &codebook[c1];
    const codebook555_t* e2 = &codebook[c2];
    const codebook555_t* e3 = &codebook[c3];

    const uint32_t x = data->writeX;
    const uint32_t y = data->writeY;

    uint32_t imageIndex = ((videoStartY + y) * VIDEO_WIDTH) + x;

    // +------+------+------+------+
    // | e0y0 | e0y1 | e1y0 | e1y1 |
    // +------+------+------+------+
    // | e0y2 | e0y3 | e1y2 | e1y3 |
    // +------+------+------+------+
    // | e2y0 | e2y1 | e3y0 | e3y1 |
    // +------+------+------+------+
    // | e2y2 | e2y3 | e3y2 | e3y3 |
    // +------+------+------+------+
    if (y + 0 >= data->bottomY)
        return;

    vdp2ImagePtr[imageIndex++] = e0->color[0];
    vdp2ImagePtr[imageIndex++] = e0->color[1];
    vdp2ImagePtr[imageIndex++] = e1->color[0];
    vdp2ImagePtr[imageIndex] = e1->color[1];

    if (y + 1 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH - 3;
    vdp2ImagePtr[imageIndex++] = e0->color[2];
    vdp2ImagePtr[imageIndex++] = e0->color[3];
    vdp2ImagePtr[imageIndex++] = e1->color[2];
    vdp2ImagePtr[imageIndex] = e1->color[3];

    if (y + 2 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH - 3;
    vdp2ImagePtr[imageIndex++] = e2->color[0];
    vdp2ImagePtr[imageIndex++] = e2->color[1];
    vdp2ImagePtr[imageIndex++] = e3->color[0];
    vdp2ImagePtr[imageIndex] = e3->color[1];

    if (y + 3 >= data->bottomY)
        return;

    imageIndex += VIDEO_WIDTH - 3;
    vdp2ImagePtr[imageIndex++] = e2->color[2];
    vdp2ImagePtr[imageIndex++] = e2->color[3];
    vdp2ImagePtr[imageIndex++] = e3->color[2];
    vdp2ImagePtr[imageIndex] = e3->color[3];
}

static void readVectors(
    binary_stream_t* stream, stripdata_t* data, uint16_t chunkDataLength) {

    uint32_t flags = stream_read32(stream);
    uint32_t remainingSectionBytes = chunkDataLength - 4;

    MOVIE_GUARD(remainingSectionBytes > TMP_BUFFER_SIZE, MOVIE_CHUNK_SHORT);
    const uint8_t* tmpData = stream_view(stream, remainingSectionBytes);
    while (remainingSectionBytes) {
        if (data->writeY >= data->bottomY)
            break;

        for (uint32_t i = 0; i < 32; ++i) {
            if (flags & 0x80000000) {
                if (remainingSectionBytes < 4)
                    break;

                // V4
                renderPixel4(
                    data, tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
                remainingSectionBytes -= 4;
                tmpData += 4;

            } else {
                if (remainingSectionBytes < 1)
                    break;

                // V1
                renderPixel1(data, tmpData[0]);
                remainingSectionBytes -= 1;
                tmpData += 1;
            }

            stripdata_skipBlock(&stripData);
            flags <<= 1;
        }

        if (remainingSectionBytes < 4)
            break;

        flags = be32(tmpData);
        tmpData += 4;
        remainingSectionBytes -= 4;
    }
}

static void readVectorsInter(
    binary_stream_t* stream, stripdata_t* data, uint16_t chunkDataLength) {

    MOVIE_GUARD(chunkDataLength < 4, MOVIE_CHUNK_SHORT);
    const uint8_t* tmpData = stream_view(stream, chunkDataLength);

    uint32_t remainingSectionBytes = chunkDataLength - 4;

    // We keep reading flags as long as it is possible. We first read 4
    // bytes and then we start shifting them for the VLC. Once we reach
    // the end we try to read a new flag.
    uint32_t flags = be32(tmpData);

    tmpData += 4;
    uint32_t shifts = 0;
    while (remainingSectionBytes) {
        if (data->writeY >= data->bottomY)
            break;

        // Running on VLC now, so we just keep consuming bytes (4 each time)
        // until there is nothing else. 0 => skip block, 1 => read next bit:
        // - next bit is 1 = V4
        // - next bit is 0 = V1
        if (flags & 0x80000000) {
            // We are at the last bit so we need to fetch the next flags and
            // check the first bit as if it was the next on this sequence.
            if (shifts == 31) {
                if (remainingSectionBytes < 4)
                    break;

                flags = be32(tmpData);
                tmpData += 4;
                remainingSectionBytes -= 4;

                shifts = 0;
            } else {
                flags <<= 1;
                shifts++;
            }

            if (flags & 0x80000000) {
                if (remainingSectionBytes < 4)
                    break;

                // V4
                renderPixel4(
                    data, tmpData[0], tmpData[1], tmpData[2], tmpData[3]);
                tmpData += 4;
                remainingSectionBytes -= 4;
            } else {
                if (remainingSectionBytes < 1)
                    break;

                // V1
                renderPixel1(data, tmpData[0]);
                tmpData += 1;
                remainingSectionBytes -= 1;
            }
        } else {
            carryBlock(data);
        }

        stripdata_skipBlock(&stripData);

        // If we read all the bits, we just fetch the next flags.
        if (shifts == 31) {
            if (remainingSectionBytes < 4)
                break;

            flags = be32(tmpData);
            tmpData += 4;
            remainingSectionBytes -= 4;

            shifts = 0;
        } else {
            flags <<= 1;
            shifts++;
        }
    }
}

static void readV1VectorsInChunk(
    binary_stream_t* stream, stripdata_t* data, uint16_t chunkDataLength) {

    uint32_t readBytes = 0;
    uint16_t originalWriteX = stripData.writeX;
    uint16_t originalWriteY = stripData.writeY;

    uint32_t remainingSectionBytes = chunkDataLength;
    while (remainingSectionBytes) {
        if (data->writeY >= data->bottomY)
            break;

        readBytes++;
        remainingSectionBytes -= 1;
        stripdata_skipBlock(&stripData);
    }

    stripData.writeX = originalWriteX;
    stripData.writeY = originalWriteY;

    while (readBytes > 0) {
        const uint32_t readNow = MIN(readBytes, TMP_BUFFER_SIZE);

        const uint8_t* tmpData = stream_view(stream, readNow);
        for (volatile uint32_t i = 0; i < readNow; ++i) {
            const uint8_t c0 = tmpData[i];
            renderPixel1(data, c0);
            stripdata_skipBlock(&stripData);
        }

        readBytes -= readNow;
    }

    if (remainingSectionBytes)
        stream_skip(stream, remainingSectionBytes);
}

static void readChunk(uint16_t chunkID, uint16_t chunkDataLength,
                      binary_stream_t* stream, stripdata_t* data) {

    switch (chunkID) {
    // 12 bit V4 (0x2000) or V1(0x2200)
    case 0x2000:
    case 0x2200: {
        const uint32_t book = chunkID == 0x2000 ? CB_V4 : CB_V1;
        codebook555_t* codebook555Ptr =
            chunkID == 0x2000 ? stripdata_getV4Codebook555(data)
                              : stripdata_getV1Codebook555(data);

        uint32_t remainingSectionBytes = chunkDataLength;

        const uint32_t numReads = remainingSectionBytes / 6;
        const uint32_t numReadBytes = numReads * 6;

        // A full book replaces every entry: the previous strip's is not needed.
        if (numReads >= 256)
            data->pending &= ~book;
        else
            stripdata_copyLastCodebooks_nodma(data, book);

        const uint8_t* tmpData = stream_view(stream, numReadBytes);
        for (uint32_t i = 0; i < MIN(numReads, 256); ++i) {
            codebook555_new(tmpData + 6 * i, &codebook555Ptr[i]);
        }

        remainingSectionBytes -= numReadBytes;

        // Deviant format shenanigans
        if (remainingSectionBytes)
            stream_skip(stream, remainingSectionBytes);
    } break;

    // 12 bit V4 (0x2100) or V1 (0x2300) - Update only
    case 0x2100:
    case 0x2300: {
        const uint32_t book = chunkID == 0x2100 ? CB_V4 : CB_V1;
        codebook555_t* codebook555Ptr =
            chunkID == 0x2100 ? stripdata_getV4Codebook555(data)
                              : stripdata_getV1Codebook555(data);
        uint32_t entry = 0;

        stripdata_copyLastCodebooks_nodma(data, book);

        // One view of the whole chunk (chunkDataLength is 16 bit, under
        // TMP_BUFFER_SIZE): no stream call per entry.
        const uint8_t* tmpData = stream_view(stream, chunkDataLength);
        uint32_t remainingSectionBytes = chunkDataLength;
        while (remainingSectionBytes >= 4) {
            uint32_t flags = be32(tmpData);
            tmpData += 4;
            remainingSectionBytes -= 4;

            for (uint32_t i = 0; i < 32; ++i) {
                if (entry == 256) {
                    remainingSectionBytes = 0;
                    break;
                }
                if (flags & 0x80000000) {
                    if (remainingSectionBytes < 6) {
                        remainingSectionBytes = 0;
                        break;
                    }
                    codebook555_new(tmpData, &codebook555Ptr[entry]);
                    tmpData += 6;
                    remainingSectionBytes -= 6;
                }

                entry++;
                flags <<= 1;
            }
        }
    } break;

    // 8 bit V4
    case 0x2400:
        break;

    // 8 bit V1
    case 0x2600:
        break;

    // vectors
    case 0x3000:
        stripdata_copyLastCodebooks_nodma(data, CB_V1 | CB_V4);
        readVectors(stream, data, chunkDataLength);
        break;

    // list of blocks from v1
    case 0x3100:
        stripdata_copyLastCodebooks_nodma(data, CB_V1 | CB_V4);
        readVectorsInter(stream, data, chunkDataLength);
        break;

    case 0x3200:
        stripdata_copyLastCodebooks_nodma(data, CB_V1 | CB_V4);
        readV1VectorsInChunk(stream, data, chunkDataLength);
        break;

    default: {
        memset(vdp2ImagePtr, 0, VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint16_t));
        break;
    }
    }
}

static void parseVideo(binary_stream_t* stream) {
    // videoHeader: flags and cvid length (u32), width, height, numStrips,
    // padding (BE u16)
    uint8_t cvidHeader[12] = {0};
    stream_readbytes(stream, cvidHeader, sizeof(cvidHeader));

    const bool copyLastCodeBooks = !(cvidHeader[0] & 0x1);
    const uint16_t numStrips = be16(cvidHeader + 8);

    uint16_t lastBottomY = 0;
    for (uint16_t stripId = 0; stripId < numStrips; ++stripId) {
        movie_tick();
        stripData.strip = stripId;

        // flag bit 0 will tell if we need the contents of the previous strip;
        // the copy waits for the first chunk that reads or patches a book.
        stripData.pending =
            stripId > 0 && copyLastCodeBooks ? CB_V1 | CB_V4 : 0;

        uint8_t stripHeader[12] = {0};
        uint16_t tmpStripBuffer[6];
        stream_readbytes(stream, stripHeader, sizeof(stripHeader));
        for (int w = 0; w < 6; ++w)
            tmpStripBuffer[w] = be16(stripHeader + 2 * w);

        const uint16_t stripDataLength = tmpStripBuffer[1];
        MOVIE_GUARD(stripDataLength < 12, MOVIE_STRIP_SHORT); // guard:strip
        if (film_status != MOVIE_END)
            return;

        stripData.topY = stripData.writeY = tmpStripBuffer[2];
        stripData.topX = stripData.writeX = tmpStripBuffer[3];
        stripData.bottomY = tmpStripBuffer[4];
        stripData.bottomX = tmpStripBuffer[5];

        if (stripId > 0 && stripData.topY == 0) {
            stripData.topY = stripData.writeY = lastBottomY;
            stripData.bottomY += lastBottomY;
        }

        // Read the strip chunks
        const uint32_t stripLimit = stream_pos(stream) + stripDataLength - 12;
        lastBottomY = stripData.bottomY;

        while (stream_pos(stream) < stripLimit) {
            movie_tick();
            const uint16_t cvidChunkID = stream_read16(stream);
            const uint16_t rawLength = stream_read16(stream);
            MOVIE_GUARD(rawLength < 4, MOVIE_CHUNK_SHORT); // guard:chunk
            const uint16_t cvidChunkDataLength = rawLength - 4;
            if (film_status != MOVIE_END)
                return;

            if (cvidChunkDataLength)
                readChunk(cvidChunkID, cvidChunkDataLength, stream, &stripData);
        } // Strip data
        stripdata_copyLastCodebooks_nodma(&stripData, CB_V1 | CB_V4);

        // Blocks past the strip's last coded one are skipped too: carry them.
        while (stripData.writeY < stripData.bottomY) {
            carryBlock(&stripData);
            stripdata_skipBlock(&stripData);
        }
    }
}

static void parseAudio(const film_sample_t* sample, binary_stream_t* stream) {
    uint32_t length = sample->length;

    // TODO: Proper stereo
    if (audioChannels == 2) {
        length >>= 1;
        stream_skip(stream, sample->length - length);
    }

    movie_tick();
    if (film_status == MOVIE_END && movie_audio_chunk(length) != 0)
        film_status = MOVIE_STOPPED;

    stream->offset += length;
    audioNumPlayedSamples++;
}

static inline void parseSample(
    const film_sample_t* sample, binary_stream_t* stream) {
    if (film_sample_is_audio(sample)) {
        parseAudio(sample, stream);
    } else {
        parseVideo(stream);
    }
}

/* Header checks: the first failure sets MOVIE_BAD_HEADER, after which the
 * stream reads nothing; the walk returns before the sample cache is filled. */
#define HEADER_REQUIRE(cond)                                                   \
    do {                                                                       \
        if (film_status == MOVIE_END && !(cond))                               \
            film_status = MOVIE_BAD_HEADER;                                    \
    } while (0)

int movie_decoder_play(
    uint32_t file_size, film_sample_t* sampleCache, uint32_t sampleCacheSize) {

    film_status = MOVIE_END;
    stripdata_new(&stripData);

    if (file_size < FILM_SAMPLE_START_OFFSET)
        return MOVIE_BAD_HEADER;

    // Start reading the file
    binary_stream_t stream;
    stream_new(&stream, file_size, sampleCache, sampleCacheSize);

    const uint32_t asciiFilm = stream_read32(&stream);
    HEADER_REQUIRE(asciiFilm == ASCII_FILM);
    const uint32_t filmHeaderLength = stream_read32(&stream);
    HEADER_REQUIRE(filmHeaderLength >= FILM_SAMPLE_START_OFFSET &&
                   filmHeaderLength <= file_size);

    const uint32_t filmVersion = stream_read32(&stream);
    HEADER_REQUIRE(filmVersion == ASCII_1d09);

    stream_skip(&stream, 4); // Unknown

    const uint32_t asciiFdsc = stream_read32(&stream);
    HEADER_REQUIRE(asciiFdsc == ASCII_FDSC);

    // FDSC length.
    stream_skip(&stream, 4);

    const uint32_t asciiCvid = stream_read32(&stream);
    HEADER_REQUIRE(asciiCvid == ASCII_CVID);

    videoHeight = stream_read32(&stream);
    videoWidth = stream_read32(&stream);
    videoStartY = (VIDEO_HEIGHT - videoHeight) / 2;

    // A = video BPP
    // B = number of audio channels
    // C = audio sampling resolution
    // D = audio compression
    const uint32_t ABCD = stream_read32(&stream);
    audioChannels = (ABCD >> 16) & 0xFF;
    audioSamplingResolution = (ABCD >> 8) & 0xFF;
    audioCompression = ABCD & 0xFF;
    audioSamplingFrequencyHz = stream_read16(&stream);
    audioNumPlayedSamples = 0;

    // Unknown
    stream_skip(&stream, 6);

    const uint32_t asciiStab = stream_read32(&stream);
    HEADER_REQUIRE(asciiStab == ASCII_STAB);

    // STAB length.
    stream_skip(&stream, 4);

    // Frame rate base frequency (pacing: the service's VBlank schedule).
    stream_skip(&stream, 4);
    const uint32_t numSamples = stream_read32(&stream);
    HEADER_REQUIRE(numSamples <= sampleCacheSize);
    if (film_status != MOVIE_END)
        return film_status;
    stream.sampleCache.numSamples = numSamples;

    // Must be called just before the sample list
    film_sample_cache_new(&stream, numSamples);
    movie_audio_table(numSamples, audioChannels);

    uint32_t frame = 0;
    vdp2ImagePtr = videoTmpBuffer[0];
    prevImagePtr = videoTmpBuffer[1];
    for (uint32_t sampleId = 0;
         sampleId < numSamples && film_status == MOVIE_END; ++sampleId) {
        if (!film_loop_handler()) {
            film_status = MOVIE_STOPPED;
            break;
        }

        film_sample_t sample = film_sample_get_next_sample(&stream.sampleCache);
        const bool isVideo = film_sample_is_video(&sample);

        parseSample(&sample, &stream);
        if (film_status != MOVIE_END)
            break;

        if (!isVideo)
            continue;
        if (movie_frame_out(vdp2ImagePtr, frame++) != 0)
            film_status = MOVIE_STOPPED;
        prevImagePtr = vdp2ImagePtr;
        vdp2ImagePtr = videoTmpBuffer[frame & 1];
    }

    return film_status;
}
