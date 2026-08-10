// indeo_decode.h
//
// A small, self-contained Indeo Video 5 decoder: feed it the compressed bytes
// of one frame, get back a YUV410P picture. No FFmpeg build, no runtime DLL,
// no registry entries -- the decoder in this directory is compiled straight
// into the shim.
//
// Not thread safe per instance. Separate instances are independent.

#ifndef BZ15_INDEO_DECODE_H
#define BZ15_INDEO_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IndeoDecoder IndeoDecoder;

typedef struct IndeoPicture
{
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int            yPitch;
    int            uvPitch;
    int            width;
    int            height;
} IndeoPicture;

// width/height come from the AVI stream format header. Returns NULL on failure.
IndeoDecoder* IndeoDecoderCreate(int width, int height);
void          IndeoDecoderDestroy(IndeoDecoder* decoder);

// Decodes one compressed frame. Returns 1 when `picture` was filled, 0 when the
// frame carried no picture of its own (Indeo's "null" frames repeat the
// previous one), and a negative value if the frame could not be decoded.
//
// The returned pointers stay valid until the next call on this decoder.
int IndeoDecoderDecode(IndeoDecoder* decoder, const void* data, int size,
                       IndeoPicture* picture);

// Drops inter-frame history, so the next frame is decoded as if it were the
// first. Call when playback seeks.
void IndeoDecoderReset(IndeoDecoder* decoder);

#ifdef __cplusplus
}
#endif

#endif // BZ15_INDEO_DECODE_H
