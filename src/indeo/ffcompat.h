// ffcompat.h
//
// The Indeo 5 decoder in this directory is FFmpeg's, carried here unmodified
// apart from its #include lines. This header stands in for the pieces of
// libavcodec/libavutil it expects, so the decoder can be compiled straight into
// a 32-bit shim with no FFmpeg build and no runtime DLL.
//
// Only what ivi.c, ivi_dsp.c and indeo5.c actually reference is provided. Two
// pieces have to behave *exactly* like their originals or the bitstream
// desynchronises:
//
//   * the LE bitstream reader (ivi.c is compiled with BITSTREAM_READER_LE)
//   * get_vlc2 against tables built by vlc_init with VLC_INIT_OUTPUT_LE
//
// Both are derived from FFmpeg's definitions in the comments below rather than
// guessed at, and the whole decoder is checked frame-for-frame against
// ffmpeg.exe output by tools/indeo_check.
//
// Error handling differs deliberately. FFmpeg aborts the process on a failed
// av_assert0 and tolerates a VLC decoder that consumes no bits; neither is
// acceptable inside a game's menu loop. Both conditions instead longjmp back to
// IndeoAbortTarget, which drops the frame and keeps the previous one on screen.

#ifndef BZ15_INDEO_FFCOMPAT_H
#define BZ15_INDEO_FFCOMPAT_H

#include <inttypes.h>   // ivi.c logs with PRIu32
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- attributes ---------------------------------------------------------

#define av_cold
#define av_unused

// ---- error codes --------------------------------------------------------

#ifndef ENOMEM
#define ENOMEM 12
#endif

#define AVERROR(e)           (-(e))
#define AVERROR_INVALIDDATA  (-1094995529)
#define AVERROR_PATCHWELCOME (-1163346256)

// ---- logging ------------------------------------------------------------

enum
{
    AV_LOG_PANIC = 0,
    AV_LOG_ERROR = 16,
    AV_LOG_WARNING = 24,
    AV_LOG_INFO = 32,
    AV_LOG_DEBUG = 48
};

void av_log(void* avcl, int level, const char* fmt, ...);

#define ff_dlog(...) do { } while (0)

void avpriv_request_sample(void* avcl, const char* msg, ...);
void avpriv_report_missing_feature(void* avcl, const char* msg, ...);

// ---- fatal-condition escape --------------------------------------------

// Set by the decoder wrapper around each frame. A failed invariant or a
// bitstream overrun unwinds to it instead of aborting the process.
extern jmp_buf* g_indeoAbortTarget;

void ff_indeo_abort(void);

#define av_assert0(cond) do { if (!(cond)) ff_indeo_abort(); } while (0)

// ---- arithmetic helpers -------------------------------------------------

#define FFMIN(a, b)     ((a) > (b) ? (b) : (a))
#define FFMAX(a, b)     ((a) > (b) ? (a) : (b))
#define FFABS(a)        ((a) >= 0 ? (a) : (-(a)))
#define FFSIGN(a)       ((a) > 0 ? 1 : -1)
#define FFALIGN(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)

static inline int av_clip(int a, int amin, int amax)
{
    if (a < amin) return amin;
    if (a > amax) return amax;
    return a;
}

static inline uint8_t av_clip_uint8(int a)
{
    if (a & (~0xFF)) return (uint8_t)((-a) >> 31);
    return (uint8_t)a;
}

static inline unsigned av_clip_uintp2(int a, int p)
{
    if (a & ~((1 << p) - 1)) return (unsigned)((~a) >> 31 & ((1 << p) - 1));
    return (unsigned)a;
}

// ---- memory -------------------------------------------------------------

void* av_malloc(size_t size);
void* av_mallocz(size_t size);
void* av_calloc(size_t count, size_t size);
void  av_free(void* ptr);
void  av_freep(void* ptr);   // takes the address of a pointer and clears it

// ---- codec/frame stand-ins ---------------------------------------------

enum { AV_CODEC_ID_INDEO4 = 1, AV_CODEC_ID_INDEO5 = 2 };
enum { AV_PIX_FMT_YUV410P = 0 };

typedef struct AVFrame
{
    uint8_t* data[4];
    int      linesize[4];
    int      width;
    int      height;
    int      alloc_w;      // dimensions the buffers were sized for
    int      alloc_h;
} AVFrame;

typedef struct AVCodecContext
{
    void*    priv_data;
    int      width;
    int      height;
    int      pix_fmt;
    int      codec_id;
    int64_t  max_pixels;
} AVCodecContext;

typedef struct AVPacket
{
    const uint8_t* data;
    int            size;
} AVPacket;

int  ff_set_dimensions(AVCodecContext* avctx, int width, int height);
int  ff_get_buffer(AVCodecContext* avctx, AVFrame* frame, int flags);
int  av_image_check_size2(unsigned w, unsigned h, int64_t maxPixels, int pixFmt,
                          int logOffset, void* avcl);
void av_frame_unref(AVFrame* frame);
void av_frame_move_ref(AVFrame* dst, AVFrame* src);
void av_frame_free(AVFrame** frame);

// ---- one-time init ------------------------------------------------------

typedef long AVOnce;
#define AV_ONCE_INIT 0

int ff_thread_once(AVOnce* control, void (*routine)(void));

// ---- shared tables ------------------------------------------------------

extern const uint8_t ff_zigzag_direct[64];

// ---- bitstream reader ---------------------------------------------------
//
// FFmpeg's LE reader is defined by
//
//     cache = AV_RL32(buffer + (index >> 3)) >> (index & 7)
//     SHOW_UBITS(n) = low n bits of cache
//
// so bit k of an n-bit read is stream bit (index + k), and stream bit b is
// buffer[b >> 3] >> (b & 7) & 1. That is exactly what read_bits_le does, which
// makes this reader bit-identical without depending on 32-bit word loads
// staying inside the buffer.

typedef struct GetBitContext
{
    const uint8_t* buffer;
    int            index;
    int            size_in_bits;
} GetBitContext;

static inline unsigned get_bit_at(const GetBitContext* s, int bitIndex)
{
    if (bitIndex < 0 || bitIndex >= s->size_in_bits)
        return 0;   // FFmpeg relies on zero padding past the end
    return (unsigned)((s->buffer[bitIndex >> 3] >> (bitIndex & 7)) & 1);
}

static inline int init_get_bits(GetBitContext* s, const uint8_t* buffer, int bitSize)
{
    if (!buffer || bitSize < 0)
        return AVERROR_INVALIDDATA;
    s->buffer = buffer;
    s->size_in_bits = bitSize;
    s->index = 0;
    return 0;
}

static inline int init_get_bits8(GetBitContext* s, const uint8_t* buffer, int byteSize)
{
    if (byteSize < 0 || byteSize > INT32_MAX / 8)
        return AVERROR_INVALIDDATA;
    return init_get_bits(s, buffer, byteSize * 8);
}

static inline unsigned int get_bits(GetBitContext* s, int n)
{
    unsigned value = 0;
    int i;
    for (i = 0; i < n; i++)
        value |= get_bit_at(s, s->index + i) << i;
    s->index += n;
    return value;
}

static inline unsigned int get_bits_long(GetBitContext* s, int n)
{
    return get_bits(s, n);
}

static inline unsigned int get_bits1(GetBitContext* s)
{
    const unsigned value = get_bit_at(s, s->index);
    s->index++;
    return value;
}

static inline unsigned int show_bits(GetBitContext* s, int n)
{
    unsigned value = 0;
    int i;
    for (i = 0; i < n; i++)
        value |= get_bit_at(s, s->index + i) << i;
    return value;
}

static inline void skip_bits(GetBitContext* s, int n)
{
    s->index += n;
}

static inline void skip_bits_long(GetBitContext* s, int n)
{
    s->index += n;
}

static inline int get_bits_count(const GetBitContext* s)
{
    return s->index;
}

static inline int get_bits_left(const GetBitContext* s)
{
    return s->size_in_bits - s->index;
}

static inline void align_get_bits(GetBitContext* s)
{
    const int n = (-s->index) & 7;
    if (n)
        s->index += n;
}

// ---- VLC ----------------------------------------------------------------
//
// FFmpeg builds these with VLC_INIT_OUTPUT_LE, which stores each code bit
// reversed so that an LE peek -- whose LSB is the *first* bit off the stream --
// indexes the table directly. The net rule is: the first bit read from the
// stream is the code's most significant bit. So a table indexed by bits
// assembled MSB-first decodes the same codes, and stays a single flat lookup
// because ivi_create_huff_from_desc rejects any code longer than IVI_VLC_BITS.

#define VLC_INIT_USE_STATIC  1
#define VLC_INIT_OUTPUT_LE   2

// Packed as symbol | (length << 8). Length 0 means "no code here".
typedef uint16_t VLCElem;

typedef struct VLC
{
    int      bits;
    VLCElem* table;
    int      table_size;
    int      table_allocated;
} VLC;

int  vlc_init(VLC* vlc, int nbBits, int nbCodes,
              const void* bits, int bitsWrap, int bitsSize,
              const void* codes, int codesWrap, int codesSize, int flags);
void ff_vlc_free(VLC* vlc);

static inline int get_vlc2(GetBitContext* s, const VLCElem* table, int bits, int maxDepth)
{
    unsigned peek = 0;
    int i;
    int length;

    (void)maxDepth;

    // Past the end of the frame there is no code to find. FFmpeg would keep
    // reading zero padding; here it means the stream is corrupt, and returning
    // a symbol that consumes no bits would spin ivi_decode_coded_blocks
    // forever, so unwind instead.
    if (s->index >= s->size_in_bits)
        ff_indeo_abort();

    for (i = 0; i < bits; i++)
        peek = (peek << 1) | get_bit_at(s, s->index + i);

    length = table[peek] >> 8;
    if (!length)
        ff_indeo_abort();

    s->index += length;
    return table[peek] & 0xFF;
}

#ifdef __cplusplus
}
#endif

#endif // BZ15_INDEO_FFCOMPAT_H
