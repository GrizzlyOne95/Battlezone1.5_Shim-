// ffcompat.c -- see ffcompat.h for what this stands in for and why.

#include "ffcompat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

jmp_buf* g_indeoAbortTarget = NULL;

void ff_indeo_abort(void)
{
    if (g_indeoAbortTarget)
        longjmp(*g_indeoAbortTarget, 1);

    // Nothing has claimed the frame, so there is no safe way to continue past a
    // broken invariant. This is unreachable while decoding goes through
    // IndeoDecodeFrame, which always installs a target.
    abort();
}

// ---- logging ------------------------------------------------------------

// Defined in indeo_log.c so this file stays free of the shim's C++ headers.
void IndeoLogLine(const char* text);

static void LogFormatted(int level, const char* fmt, va_list args)
{
    char text[512];

    if (level > AV_LOG_ERROR)
        return;   // the decoder is chatty at DEBUG; only failures are useful

    vsnprintf(text, sizeof(text), fmt, args);
    text[sizeof(text) - 1] = '\0';
    IndeoLogLine(text);
}

void av_log(void* avcl, int level, const char* fmt, ...)
{
    va_list args;
    (void)avcl;
    va_start(args, fmt);
    LogFormatted(level, fmt, args);
    va_end(args);
}

void avpriv_request_sample(void* avcl, const char* msg, ...)
{
    va_list args;
    (void)avcl;
    va_start(args, msg);
    LogFormatted(AV_LOG_ERROR, msg, args);
    va_end(args);
}

void avpriv_report_missing_feature(void* avcl, const char* msg, ...)
{
    va_list args;
    (void)avcl;
    va_start(args, msg);
    LogFormatted(AV_LOG_ERROR, msg, args);
    va_end(args);
}

// ---- memory -------------------------------------------------------------

void* av_malloc(size_t size)
{
    if (!size)
        size = 1;
    return malloc(size);
}

void* av_mallocz(size_t size)
{
    void* ptr = av_malloc(size);
    if (ptr)
        memset(ptr, 0, size ? size : 1);
    return ptr;
}

void* av_calloc(size_t count, size_t size)
{
    if (count && size > (size_t)-1 / count)
        return NULL;
    return calloc(count ? count : 1, size ? size : 1);
}

void av_free(void* ptr)
{
    free(ptr);
}

void av_freep(void* ptr)
{
    void** slot = (void**)ptr;
    if (slot)
    {
        free(*slot);
        *slot = NULL;
    }
}

// ---- frames -------------------------------------------------------------

int av_image_check_size2(unsigned w, unsigned h, int64_t maxPixels, int pixFmt,
                         int logOffset, void* avcl)
{
    (void)pixFmt;
    (void)logOffset;
    (void)avcl;

    if (!w || !h || w > 16384 || h > 16384)
        return AVERROR_INVALIDDATA;
    if (maxPixels > 0 && (int64_t)w * (int64_t)h > maxPixels)
        return AVERROR_INVALIDDATA;
    return 0;
}

int ff_set_dimensions(AVCodecContext* avctx, int width, int height)
{
    if (av_image_check_size2((unsigned)width, (unsigned)height,
                             avctx->max_pixels, AV_PIX_FMT_YUV410P, 0, avctx) < 0)
    {
        return AVERROR_INVALIDDATA;
    }

    avctx->width = width;
    avctx->height = height;
    return 0;
}

void av_frame_unref(AVFrame* frame)
{
    if (!frame)
        return;

    av_freep(&frame->data[0]);
    frame->data[1] = NULL;
    frame->data[2] = NULL;
    frame->data[3] = NULL;
    memset(frame->linesize, 0, sizeof(frame->linesize));
    frame->width = frame->height = 0;
    frame->alloc_w = frame->alloc_h = 0;
}

void av_frame_move_ref(AVFrame* dst, AVFrame* src)
{
    if (!dst || !src)
        return;
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

void av_frame_free(AVFrame** frame)
{
    if (!frame || !*frame)
        return;
    av_frame_unref(*frame);
    av_freep(frame);
}

// YUV410P: chroma is subsampled 4x in both directions. One allocation backs all
// three planes so a frame is a single malloc, and it is reused while the
// dimensions hold -- MCI decodes hundreds of frames per clip.
int ff_get_buffer(AVCodecContext* avctx, AVFrame* frame, int flags)
{
    int lumaStride, chromaStride, chromaHeight;
    size_t lumaBytes, chromaBytes;
    uint8_t* block;

    (void)flags;

    if (!frame)
        return AVERROR(ENOMEM);

    if (frame->data[0] && frame->alloc_w == avctx->width &&
        frame->alloc_h == avctx->height)
    {
        frame->width = avctx->width;
        frame->height = avctx->height;
        return 0;
    }

    av_frame_unref(frame);

    lumaStride = FFALIGN(avctx->width, 16);
    chromaStride = FFALIGN((avctx->width + 3) >> 2, 16);
    chromaHeight = (avctx->height + 3) >> 2;

    lumaBytes = (size_t)lumaStride * (size_t)avctx->height;
    chromaBytes = (size_t)chromaStride * (size_t)chromaHeight;

    block = (uint8_t*)av_mallocz(lumaBytes + chromaBytes * 2);
    if (!block)
        return AVERROR(ENOMEM);

    frame->data[0] = block;
    frame->data[1] = block + lumaBytes;
    frame->data[2] = block + lumaBytes + chromaBytes;
    frame->data[3] = NULL;
    frame->linesize[0] = lumaStride;
    frame->linesize[1] = chromaStride;
    frame->linesize[2] = chromaStride;
    frame->linesize[3] = 0;
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->alloc_w = avctx->width;
    frame->alloc_h = avctx->height;
    return 0;
}

// ---- one-time init ------------------------------------------------------

int ff_thread_once(AVOnce* control, void (*routine)(void))
{
    // 0 = untouched, 1 = in progress, 2 = published. MCI can open two clips on
    // different threads, so the static VLC tables have to be built exactly once.
    if (*control == 2)
        return 0;

    if (InterlockedCompareExchange((long volatile*)control, 1, 0) == 0)
    {
        routine();
        InterlockedExchange((long volatile*)control, 2);
        return 0;
    }

    while (*control != 2)
        Sleep(0);
    return 0;
}

// ---- shared tables ------------------------------------------------------

const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// ---- VLC ----------------------------------------------------------------

int vlc_init(VLC* vlc, int nbBits, int nbCodes,
             const void* bits, int bitsWrap, int bitsSize,
             const void* codes, int codesWrap, int codesSize, int flags)
{
    const int tableSize = 1 << nbBits;
    int i;

    if (nbBits <= 0 || nbBits > 16 || nbCodes < 0 || nbCodes > 256)
        return AVERROR_INVALIDDATA;
    if (bitsSize != 1 || codesSize != 2)
        return AVERROR_INVALIDDATA;   // the only shape ivi.c uses

    if (flags & VLC_INIT_USE_STATIC)
    {
        if (!vlc->table || vlc->table_allocated < tableSize)
            return AVERROR_INVALIDDATA;
    }
    else
    {
        av_freep(&vlc->table);
        vlc->table = (VLCElem*)av_malloc((size_t)tableSize * sizeof(VLCElem));
        if (!vlc->table)
            return AVERROR(ENOMEM);
        vlc->table_allocated = tableSize;
    }

    vlc->bits = nbBits;
    vlc->table_size = tableSize;
    memset(vlc->table, 0, (size_t)tableSize * sizeof(VLCElem));

    for (i = 0; i < nbCodes; i++)
    {
        const uint8_t length = *((const uint8_t*)bits + (size_t)i * bitsWrap);
        unsigned code;
        unsigned base;
        unsigned span;
        unsigned j;

        if (!length)
            continue;
        if (length > nbBits)
        {
            if (!(flags & VLC_INIT_USE_STATIC))
                av_freep(&vlc->table);
            return AVERROR_INVALIDDATA;
        }

        code = *(const uint16_t*)((const uint8_t*)codes + (size_t)i * codesWrap);
        if (code >= (1u << length))
            continue;   // not a code of this length; ivi never generates one

        base = code << (nbBits - length);
        span = 1u << (nbBits - length);
        for (j = 0; j < span; j++)
            vlc->table[base + j] = (VLCElem)((i & 0xFF) | (length << 8));
    }

    return 0;
}

void ff_vlc_free(VLC* vlc)
{
    if (!vlc)
        return;
    av_freep(&vlc->table);
    vlc->table_allocated = 0;
    vlc->table_size = 0;
}
