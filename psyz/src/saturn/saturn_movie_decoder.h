/* saturn_movie_decoder.h -- Cinepak FILM decoder vendored from libyaul_cinepak
 * c186f56 decoder.h (MIT, Copyright (c) 2022-2025 Romulo Fernandes; the deltas
 * and the license text are in saturn_movie_decoder.c). Platform-free: the
 * stream, the display and the sound go through the four seams below, which the
 * movie service or the host harness implements.
 */
#ifndef SATURN_MOVIE_DECODER_H
#define SATURN_MOVIE_DECODER_H

#include <stdint.h>

#define FILM_SAMPLE_START_OFFSET 64

#define ASCII_FILM 1179208781 // 'FILM'
#define ASCII_1d09 825110585  // '1.09'
#define ASCII_FDSC 1178882883 // 'FDSC'
#define ASCII_CVID 1668704612 // 'cvid'
#define ASCII_STAB 1398030658 // 'STAB'

/* movie_decoder_play statuses */
#define MOVIE_END 0
#define MOVIE_CHUNK_SHORT 1 /* raw chunk length < 4, or a tmpBuffer overrun */
#define MOVIE_STRIP_SHORT 2 /* strip length < 12 */
#define MOVIE_BAD_HEADER 3  /* magic, version, sample count, short file */
#define MOVIE_STOPPED 4     /* a seam or film_loop_handler returned stop */

// Sample data can be stored as a single uint32_t with the MSB telling us if its
// a video (0) or audio sample (1). If it is a video sample, the rest of the
// data holds the 'info2' field from the STAB data and if its audio, it holds
// the sample length.
typedef struct {
    uint32_t interval; // 0xFFFFFFFF if audio
    uint32_t length;
} film_sample_t;

/* Seams. movie_stream_refill: the next n file bytes in file order into dst
 * (dst NULL: skip them). movie_frame_out: 320x240 frame k (rows centred).
 * movie_audio_chunk: consume the next n stream bytes as PCM. movie_tick: called
 * at every strip and chunk boundary. A nonzero return stops the movie. */
int movie_stream_refill(void* dst, uint32_t n);
/* movie_stream_view: the next n file bytes in place, consumed; valid until the
 * next stream call. NULL (nothing consumed) when they are not contiguous in the
 * stream's buffer: the decoder then uses movie_stream_refill. */
const uint8_t* movie_stream_view(uint32_t n);
int movie_frame_out(const uint16_t* rgb555, uint32_t k);
int movie_audio_chunk(uint32_t n);
/* movie_audio_table: called once before frame 0 with the parsed sample table
 * (cache[0..n)); mono sound may be written ahead from it. */
void movie_audio_table(uint32_t n, uint32_t channels);
void movie_tick(void);

// Return 0 to stop film playback
int film_loop_handler(void);

/* Plays a FILM of file_size bytes from offset 0 of the stream; cache holds
 * cache_n samples. Returns a MOVIE_* status. */
int movie_decoder_play(
    uint32_t file_size, film_sample_t* cache, uint32_t cache_n);

/* Strip codebooks copied from the previous strip since start-up (a
 * statistic). */
extern uint32_t movie_decoder_cbcopies;

#endif // SATURN_MOVIE_DECODER_H
