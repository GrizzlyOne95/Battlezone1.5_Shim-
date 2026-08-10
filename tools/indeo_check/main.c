// indeo_check -- proves the vendored decoder is bit-identical to FFmpeg's.
//
//     indeo_check <clip.avi> <reference.yuv>
//
// where reference.yuv came from
//
//     ffmpeg -i clip.avi -pix_fmt yuv410p -f rawvideo reference.yuv
//
// Every decoded plane is compared byte for byte against the reference. Anything
// short of an exact match is a bug in the port, not a tolerance to widen: both
// sides are running the same integer transforms on the same bitstream.

#include "indeo_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void IndeoLogLine(const char* text)
{
    fprintf(stderr, "  [decoder] %s", text);
}

static unsigned ReadU32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

#define FOURCC(a, b, c, d) \
    ((unsigned)(a) | ((unsigned)(b) << 8) | ((unsigned)(c) << 16) | ((unsigned)(d) << 24))

static unsigned char* ReadWholeFile(const char* path, size_t* sizeOut)
{
    FILE* file = fopen(path, "rb");
    unsigned char* data;
    long size;

    if (!file)
        return NULL;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(file);
        return NULL;
    }

    data = (unsigned char*)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size)
    {
        free(data);
        fclose(file);
        return NULL;
    }

    fclose(file);
    *sizeOut = (size_t)size;
    return data;
}

// Locate the video stream format and the movi list. Only what this tool needs.
static int ParseAvi(const unsigned char* data, size_t size,
                    int* width, int* height, unsigned* handler,
                    size_t* moviBegin, size_t* moviEnd)
{
    size_t off;

    if (size < 32 || ReadU32(data) != FOURCC('R', 'I', 'F', 'F') ||
        ReadU32(data + 8) != FOURCC('A', 'V', 'I', ' '))
    {
        return 0;
    }

    *width = *height = 0;
    *moviBegin = *moviEnd = 0;

    for (off = 12; off + 8 <= size;)
    {
        const unsigned id = ReadU32(data + off);
        const unsigned chunkSize = ReadU32(data + off + 4);
        size_t advance;

        if (id == FOURCC('L', 'I', 'S', 'T') && off + 12 <= size)
        {
            const unsigned listType = ReadU32(data + off + 8);

            if (listType == FOURCC('m', 'o', 'v', 'i'))
            {
                *moviBegin = off + 12;
                *moviEnd = off + 8 + chunkSize;
                if (*moviEnd > size)
                    *moviEnd = size;
            }

            if (listType == FOURCC('h', 'd', 'r', 'l') ||
                listType == FOURCC('s', 't', 'r', 'l'))
            {
                // Descend into header lists rather than skipping them.
                off += 12;
                continue;
            }
        }
        else if (id == FOURCC('s', 't', 'r', 'f') && !*width && off + 8 + 40 <= size)
        {
            *width = (int)ReadU32(data + off + 8 + 4);
            *height = (int)ReadU32(data + off + 8 + 8);
            *handler = ReadU32(data + off + 8 + 16);
        }

        advance = 8 + (size_t)chunkSize + (chunkSize & 1);
        if (advance <= 8)
            break;
        off += advance;
    }

    return *width > 0 && *height != 0 && *moviBegin != 0;
}

int main(int argc, char** argv)
{
    const char* aviPath;
    const char* refPath;
    unsigned char* avi;
    unsigned char* reference = NULL;
    size_t aviSize = 0;
    size_t refSize = 0;
    size_t moviBegin, moviEnd, off;
    size_t refOffset = 0;
    int width = 0, height = 0;
    unsigned handler = 0;
    IndeoDecoder* decoder;
    int chunkCount = 0, pictureCount = 0, nullCount = 0, failCount = 0;
    int mismatches = 0;
    size_t frameBytes, chromaW, chromaH;
    FILE* dump = NULL;   // optional raw yuv410p output, for diagnosing a mismatch

    if (argc < 3)
    {
        fprintf(stderr, "usage: indeo_check <clip.avi> <reference.yuv> [dump.yuv]\n");
        return 2;
    }

    aviPath = argv[1];
    refPath = argv[2];
    if (argc > 3)
    {
        dump = fopen(argv[3], "wb");
        if (!dump)
        {
            fprintf(stderr, "cannot write %s\n", argv[3]);
            return 2;
        }
    }

    avi = ReadWholeFile(aviPath, &aviSize);
    if (!avi)
    {
        fprintf(stderr, "cannot read %s\n", aviPath);
        return 2;
    }

    if (!ParseAvi(avi, aviSize, &width, &height, &handler, &moviBegin, &moviEnd))
    {
        fprintf(stderr, "%s is not an AVI this tool understands\n", aviPath);
        free(avi);
        return 2;
    }

    if (height < 0)
        height = -height;   // top-down DIB

    reference = ReadWholeFile(refPath, &refSize);
    if (!reference)
    {
        fprintf(stderr, "cannot read %s\n", refPath);
        free(avi);
        return 2;
    }

    chromaW = (size_t)((width + 3) >> 2);
    chromaH = (size_t)((height + 3) >> 2);
    frameBytes = (size_t)width * (size_t)height + chromaW * chromaH * 2;

    printf("%s: %dx%d handler=%c%c%c%c  reference=%u frames\n",
           aviPath, width, height,
           (char)(handler & 0xFF), (char)((handler >> 8) & 0xFF),
           (char)((handler >> 16) & 0xFF), (char)((handler >> 24) & 0xFF),
           (unsigned)(refSize / frameBytes));

    decoder = IndeoDecoderCreate(width, height);
    if (!decoder)
    {
        fprintf(stderr, "decoder init failed\n");
        free(avi);
        free(reference);
        return 2;
    }

    for (off = moviBegin; off + 8 <= moviEnd;)
    {
        const unsigned id = ReadU32(avi + off);
        const unsigned chunkSize = ReadU32(avi + off + 4);
        const unsigned char* payload = avi + off + 8;
        size_t advance;
        int isVideo;

        // outro.avi groups each frame's video and audio into a "LIST rec ",
        // so the interesting chunks are one level down.
        if (id == FOURCC('L', 'I', 'S', 'T') && off + 12 <= moviEnd)
        {
            off += 12;
            continue;
        }

        // Stream 0 video: "00dc" (compressed) or "00db" (uncompressed).
        isVideo = (id == FOURCC('0', '0', 'd', 'c') ||
                   id == FOURCC('0', '0', 'd', 'b'));

        // credits.avi carries zero-length video chunks, which mean "hold the
        // previous frame". FFmpeg emits nothing for them, so neither do we.
        if (isVideo && chunkSize > 0 && off + 8 + chunkSize <= moviEnd)
        {
            IndeoPicture picture;
            int got;

            chunkCount++;
            got = IndeoDecoderDecode(decoder, payload, (int)chunkSize, &picture);

            if (got < 0)
            {
                if (failCount < 5)
                    printf("  frame %d: DECODE FAILED (%u bytes)\n", chunkCount, chunkSize);
                failCount++;
            }
            else if (got == 0)
            {
                nullCount++;
            }
            else
            {
                pictureCount++;

                if (dump)
                {
                    int row;
                    for (row = 0; row < height; row++)
                        fwrite(picture.y + (size_t)row * picture.yPitch, 1, (size_t)width, dump);
                    for (row = 0; row < (int)chromaH; row++)
                        fwrite(picture.u + (size_t)row * picture.uvPitch, 1, chromaW, dump);
                    for (row = 0; row < (int)chromaH; row++)
                        fwrite(picture.v + (size_t)row * picture.uvPitch, 1, chromaW, dump);
                }

                if (refOffset + frameBytes <= refSize)
                {
                    const unsigned char* refY = reference + refOffset;
                    const unsigned char* refU = refY + (size_t)width * (size_t)height;
                    const unsigned char* refV = refU + chromaW * chromaH;
                    int bad = 0;
                    int y;

                    for (y = 0; y < height && !bad; y++)
                    {
                        if (memcmp(picture.y + (size_t)y * picture.yPitch,
                                   refY + (size_t)y * width, (size_t)width) != 0)
                        {
                            printf("  frame %d: luma row %d differs\n", pictureCount, y);
                            bad = 1;
                        }
                    }
                    for (y = 0; y < (int)chromaH && !bad; y++)
                    {
                        if (memcmp(picture.u + (size_t)y * picture.uvPitch,
                                   refU + (size_t)y * chromaW, chromaW) != 0)
                        {
                            printf("  frame %d: U row %d differs\n", pictureCount, y);
                            bad = 1;
                        }
                        if (memcmp(picture.v + (size_t)y * picture.uvPitch,
                                   refV + (size_t)y * chromaW, chromaW) != 0)
                        {
                            printf("  frame %d: V row %d differs\n", pictureCount, y);
                            bad = 1;
                        }
                    }

                    if (bad && ++mismatches >= 5 && !dump)
                    {
                        printf("  ...stopping after 5 mismatching frames\n");
                        break;
                    }
                    refOffset += frameBytes;
                }
            }
        }

        advance = 8 + (size_t)chunkSize + (chunkSize & 1);
        off += advance;
    }

    IndeoDecoderDestroy(decoder);
    if (dump)
        fclose(dump);
    free(avi);
    free(reference);

    printf("  chunks=%d pictures=%d null=%d failed=%d mismatching=%d  reference frames consumed=%u/%u\n",
           chunkCount, pictureCount, nullCount, failCount, mismatches,
           (unsigned)(refOffset / frameBytes), (unsigned)(refSize / frameBytes));

    if (mismatches)
    {
        printf("  RESULT: FAIL\n");
        return 1;
    }
    if (pictureCount == 0)
    {
        printf("  RESULT: FAIL (no pictures decoded)\n");
        return 1;
    }

    // Refusing a frame is only correct if FFmpeg refused it too -- intro.avi
    // ships twelve two-byte chunks that neither decoder accepts. That is what
    // this check is for: had we rejected a frame FFmpeg decoded (or vice versa)
    // the reference would no longer line up, and every later frame would
    // mismatch rather than silently pass.
    if (refOffset != refSize)
    {
        printf("  RESULT: FAIL (decoded %u frames, reference has %u)\n",
               (unsigned)(refOffset / frameBytes), (unsigned)(refSize / frameBytes));
        return 1;
    }

    if (failCount)
        printf("  RESULT: exact match (%d unreadable frames, refused by FFmpeg too)\n", failCount);
    else
        printf("  RESULT: exact match\n");
    return 0;
}
