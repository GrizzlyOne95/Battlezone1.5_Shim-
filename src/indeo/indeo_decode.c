// indeo_decode.c -- the shim's front door to the vendored FFmpeg decoder.

#include "indeo_decode.h"

#include "ffcompat.h"
#include "ivi.h"

#include <stdlib.h>

int ff_indeo5_decode_init(AVCodecContext* avctx);

struct IndeoDecoder
{
    AVCodecContext avctx;
    AVFrame        frame;
    int            width;
    int            height;
    int            initialised;
};

static int InitCodec(IndeoDecoder* decoder)
{
    decoder->avctx.width = decoder->width;
    decoder->avctx.height = decoder->height;
    decoder->avctx.codec_id = AV_CODEC_ID_INDEO5;
    decoder->avctx.max_pixels = 0;   // the caller already vetted the dimensions

    memset(decoder->avctx.priv_data, 0, sizeof(IVI45DecContext));

    if (ff_indeo5_decode_init(&decoder->avctx) < 0)
        return -1;

    decoder->initialised = 1;
    return 0;
}

IndeoDecoder* IndeoDecoderCreate(int width, int height)
{
    IndeoDecoder* decoder;
    jmp_buf abortTarget;
    jmp_buf* saved;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return NULL;

    decoder = (IndeoDecoder*)av_mallocz(sizeof(*decoder));
    if (!decoder)
        return NULL;

    decoder->width = width;
    decoder->height = height;
    decoder->avctx.priv_data = av_mallocz(sizeof(IVI45DecContext));
    if (!decoder->avctx.priv_data)
    {
        av_free(decoder);
        return NULL;
    }

    // Building the static VLC tables runs decoder code, so it needs a target too.
    saved = g_indeoAbortTarget;
    if (setjmp(abortTarget) != 0)
    {
        g_indeoAbortTarget = saved;
        av_free(decoder->avctx.priv_data);
        av_free(decoder);
        return NULL;
    }
    g_indeoAbortTarget = &abortTarget;

    if (InitCodec(decoder) < 0)
    {
        g_indeoAbortTarget = saved;
        av_free(decoder->avctx.priv_data);
        av_free(decoder);
        return NULL;
    }

    g_indeoAbortTarget = saved;
    return decoder;
}

void IndeoDecoderDestroy(IndeoDecoder* decoder)
{
    if (!decoder)
        return;

    if (decoder->initialised)
        ff_ivi_decode_close(&decoder->avctx);

    av_frame_unref(&decoder->frame);
    av_free(decoder->avctx.priv_data);
    av_free(decoder);
}

void IndeoDecoderReset(IndeoDecoder* decoder)
{
    jmp_buf abortTarget;
    jmp_buf* saved;

    if (!decoder || !decoder->initialised)
        return;

    saved = g_indeoAbortTarget;
    if (setjmp(abortTarget) != 0)
    {
        g_indeoAbortTarget = saved;
        decoder->initialised = 0;
        return;
    }
    g_indeoAbortTarget = &abortTarget;

    ff_ivi_decode_close(&decoder->avctx);
    decoder->initialised = 0;
    InitCodec(decoder);

    g_indeoAbortTarget = saved;
}

int IndeoDecoderDecode(IndeoDecoder* decoder, const void* data, int size,
                       IndeoPicture* picture)
{
    jmp_buf abortTarget;
    jmp_buf* saved;
    AVPacket packet;
    int gotFrame = 0;
    int result;

    if (!decoder || !decoder->initialised || !data || size <= 0 || !picture)
        return -1;

    packet.data = (const uint8_t*)data;
    packet.size = size;

    // A corrupt frame unwinds here rather than aborting the game. The decoder
    // context stays usable: ff_ivi_decode_frame marks the destination buffer
    // invalid before it starts, so every later frame that depends on this one
    // is refused until the next intra frame arrives.
    saved = g_indeoAbortTarget;
    if (setjmp(abortTarget) != 0)
    {
        g_indeoAbortTarget = saved;
        return -1;
    }
    g_indeoAbortTarget = &abortTarget;

    result = ff_ivi_decode_frame(&decoder->avctx, &decoder->frame, &gotFrame, &packet);

    g_indeoAbortTarget = saved;

    if (result < 0)
        return -1;
    if (!gotFrame)
        return 0;

    picture->y = decoder->frame.data[0];
    picture->u = decoder->frame.data[1];
    picture->v = decoder->frame.data[2];
    picture->yPitch = decoder->frame.linesize[0];
    picture->uvPitch = decoder->frame.linesize[1];
    picture->width = decoder->frame.width;
    picture->height = decoder->frame.height;

    if (!picture->y || picture->width <= 0 || picture->height <= 0)
        return -1;

    return 1;
}
