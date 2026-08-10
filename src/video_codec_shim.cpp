// video_codec_shim.cpp
//
// Battlezone plays every menu animation and cutscene through MCI's "AVIVideo"
// device, and mciavi32 decodes video through Video for Windows. 45 of the 52
// shipped clips are Indeo Video 5 ("IV50"); Microsoft removed the Indeo
// decoders years ago and never replaced them, so on a modern machine VfW has
// nothing that can decode them. That is the whole bug:
//
//   * anims\*.avi hold a single video stream, so MCI_OPEN itself fails with
//     error 6, "there is no driver installed on your system".
//   * movie\*.avi hold video and audio, so MCI_OPEN and MCI_PLAY both succeed,
//     the position advances, the soundtrack plays -- and no frame is ever
//     drawn, because only the video stream is undecodable.
//
// The remaining clips (posters.avi, sspil.avi, svhra.avi are Microsoft Video 1;
// bzone.avi is RLE) still play, which is why the symptom reads as "some of the
// menu videos are broken" rather than "video is broken".
//
// So rather than converting the files, this supplies the missing decoder.
// ICInstall with ICINSTALL_FUNCTION registers a decompressor for this process
// only -- no system codec, no registry write, nothing left behind when the game
// exits -- and mciavi32 then finds IV50 through the ordinary ICM lookup and
// drives it like any other codec. The stock AVI files are played untouched.
//
// The decoder itself is FFmpeg's, compiled into the shim from src/indeo; see
// src/indeo/ffcompat.h. It is checked frame-for-frame against ffmpeg.exe by
// tools/indeo_check, which currently matches bit-for-bit across all 52 clips.
//
// Deliberately not handled: ICM_DRAW_*. Claiming the drawing interface would
// make us responsible for presenting frames; refusing it leaves mciavi32 on its
// normal decompress-then-GDI-blit path, which is what the rest of the shim's
// movie handling already understands.

#include "video_codec_shim.h"

#include "indeo/indeo_decode.h"
#include "shim_log.h"

#include <Windows.h>
#include <vfw.h>
#include <cstdint>
#include <cstring>
#include <new>

namespace
{
    constexpr DWORD kFourccIV50 = mmioFOURCC('I', 'V', '5', '0');
    constexpr DWORD kFourccIv50 = mmioFOURCC('i', 'v', '5', '0');

    bool g_installed = false;

    bool IsIndeo5(DWORD fourcc)
    {
        // msvfw32 lowercases the handler on the way through ICOpen, so both
        // spellings turn up depending on whether it came from the caller or
        // from the stream's BITMAPINFOHEADER.
        return fourcc == kFourccIV50 || fourcc == kFourccIv50;
    }

    // ---- colour conversion ----------------------------------------------
    //
    // Indeo 5 decodes to YUV410P. BT.601 with limited range is what the frames
    // actually carry: converting a decoded clip both ways and comparing against
    // "ffmpeg -pix_fmt rgb24" puts limited range at a mean error of 0.5 per
    // channel (pure rounding against swscale) and full range at 14.9.

    int g_yTab[256];
    int g_rvTab[256];
    int g_guTab[256];
    int g_gvTab[256];
    int g_buTab[256];
    uint8_t g_clampTab[1024];
    bool g_tablesReady = false;

    void BuildTables()
    {
        if (g_tablesReady)
            return;

        for (int i = 0; i < 256; ++i)
        {
            g_yTab[i] = static_cast<int>((i - 16) * 1.164 * 65536.0 + 0.5);
            g_rvTab[i] = static_cast<int>((i - 128) * 1.596 * 65536.0 + 0.5);
            g_guTab[i] = static_cast<int>((i - 128) * -0.391 * 65536.0 - 0.5);
            g_gvTab[i] = static_cast<int>((i - 128) * -0.813 * 65536.0 - 0.5);
            g_buTab[i] = static_cast<int>((i - 128) * 2.018 * 65536.0 + 0.5);
        }

        // Indexed by (value >> 16) + 384, so every reachable intermediate lands
        // inside the table and the inner loop needs no branches.
        for (int i = 0; i < 1024; ++i)
        {
            const int v = i - 384;
            g_clampTab[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }

        g_tablesReady = true;
    }

    inline uint8_t Clamp(int fixed16)
    {
        return g_clampTab[static_cast<unsigned>((fixed16 >> 16) + 384) & 1023];
    }

    struct OutputFormat
    {
        int  bitCount = 0;
        LONG width = 0;
        LONG height = 0;      // as declared: positive means a bottom-up DIB
        LONG stride = 0;
    };

    void ConvertPlane(const IndeoPicture& picture, const OutputFormat& format, uint8_t* dst)
    {
        const int width = static_cast<int>(format.width);
        const int height = static_cast<int>(format.height < 0 ? -format.height : format.height);
        const bool bottomUp = format.height > 0;

        const int copyW = picture.width < width ? picture.width : width;
        const int copyH = picture.height < height ? picture.height : height;

        for (int y = 0; y < copyH; ++y)
        {
            const uint8_t* rowY = picture.y + static_cast<size_t>(y) * picture.yPitch;
            const uint8_t* rowU = picture.u + static_cast<size_t>(y >> 2) * picture.uvPitch;
            const uint8_t* rowV = picture.v + static_cast<size_t>(y >> 2) * picture.uvPitch;

            // A bottom-up DIB stores the last image row first.
            const int dstRow = bottomUp ? (height - 1 - y) : y;
            uint8_t* out = dst + static_cast<size_t>(dstRow) * format.stride;

            for (int x = 0; x < copyW; ++x)
            {
                const int luma = g_yTab[rowY[x]];
                const int u = rowU[x >> 2];
                const int v = rowV[x >> 2];

                const uint8_t r = Clamp(luma + g_rvTab[v]);
                const uint8_t g = Clamp(luma + g_gvTab[v] + g_guTab[u]);
                const uint8_t b = Clamp(luma + g_buTab[u]);

                switch (format.bitCount)
                {
                case 32:
                    out[0] = b; out[1] = g; out[2] = r; out[3] = 0;
                    out += 4;
                    break;
                case 24:
                    out[0] = b; out[1] = g; out[2] = r;
                    out += 3;
                    break;
                default:
                {
                    // 16bpp BI_RGB is 555 on Windows.
                    const uint16_t packed = static_cast<uint16_t>(
                        ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                    *reinterpret_cast<uint16_t*>(out) = packed;
                    out += 2;
                    break;
                }
                }
            }
        }
    }

    // ---- driver instance -------------------------------------------------

    constexpr DWORD kInstanceMagic = 0x35564931;   // "1IV5"

    struct Instance
    {
        DWORD         magic = kInstanceMagic;
        IndeoDecoder* decoder = nullptr;
        LONG          srcWidth = 0;
        LONG          srcHeight = 0;
        OutputFormat  output;
        bool          decoding = false;
        bool          loggedFirstFrame = false;
    };

    Instance* AsInstance(DWORD_PTR id)
    {
        auto* instance = reinterpret_cast<Instance*>(id);
        if (!instance || IsBadReadPtr(instance, sizeof(Instance)) ||
            instance->magic != kInstanceMagic)
        {
            return nullptr;
        }
        return instance;
    }

    bool SupportedOutput(const BITMAPINFOHEADER* out)
    {
        if (!out)
            return false;
        if (out->biCompression != BI_RGB)
            return false;
        return out->biBitCount == 16 || out->biBitCount == 24 || out->biBitCount == 32;
    }

    LRESULT QueryDecompress(const BITMAPINFOHEADER* in, const BITMAPINFOHEADER* out)
    {
        if (!in || !IsIndeo5(in->biCompression))
            return ICERR_BADFORMAT;
        if (in->biWidth <= 0 || in->biHeight == 0)
            return ICERR_BADFORMAT;

        if (!out)
            return ICERR_OK;   // "can you decode this input at all?"

        if (!SupportedOutput(out))
            return ICERR_BADFORMAT;

        const LONG outHeight = out->biHeight < 0 ? -out->biHeight : out->biHeight;
        const LONG inHeight = in->biHeight < 0 ? -in->biHeight : in->biHeight;
        if (out->biWidth != in->biWidth || outHeight != inHeight)
            return ICERR_BADFORMAT;   // we do not scale

        return ICERR_OK;
    }

    LRESULT BeginDecompress(Instance* instance, const BITMAPINFOHEADER* in,
                            const BITMAPINFOHEADER* out)
    {
        if (!instance)
            return ICERR_BADPARAM;
        if (QueryDecompress(in, out) != ICERR_OK)
            return ICERR_BADFORMAT;

        BuildTables();

        const LONG height = in->biHeight < 0 ? -in->biHeight : in->biHeight;

        if (instance->decoder && (instance->srcWidth != in->biWidth ||
                                  instance->srcHeight != height))
        {
            IndeoDecoderDestroy(instance->decoder);
            instance->decoder = nullptr;
        }

        if (!instance->decoder)
        {
            instance->decoder = IndeoDecoderCreate(in->biWidth, height);
            if (!instance->decoder)
            {
                ShimLog("codec: could not create an Indeo 5 decoder for %ldx%ld",
                        in->biWidth, height);
                return ICERR_MEMORY;
            }
        }
        else
        {
            IndeoDecoderReset(instance->decoder);
        }

        instance->srcWidth = in->biWidth;
        instance->srcHeight = height;
        instance->output.bitCount = out->biBitCount;
        instance->output.width = out->biWidth;
        instance->output.height = out->biHeight;
        instance->output.stride =
            ((out->biWidth * (out->biBitCount / 8)) + 3) & ~3;
        instance->decoding = true;
        instance->loggedFirstFrame = false;

        ShimLog("codec: decoding IV50 %ldx%ld -> %d bpp", in->biWidth, height,
                out->biBitCount);
        return ICERR_OK;
    }

    LRESULT Decompress(Instance* instance, const void* input, DWORD inputSize,
                       void* output, DWORD flags)
    {
        if (!instance || !instance->decoder || !instance->decoding)
            return ICERR_BADPARAM;
        if (!input || inputSize == 0)
            return ICERR_BADPARAM;

        IndeoPicture picture = {};
        const int got = IndeoDecoderDecode(instance->decoder, input,
                                           static_cast<int>(inputSize), &picture);
        if (got < 0)
            return ICERR_ERROR;

        // A null frame repeats the previous picture, and HURRYUP means the
        // caller is catching up and will not show this one. Both still had to
        // run the decode above, because later frames are coded against it.
        if (got == 0 || (flags & ICDECOMPRESS_HURRYUP))
            return ICERR_OK;

        if (!output)
            return ICERR_BADPARAM;

        ConvertPlane(picture, instance->output, static_cast<uint8_t*>(output));

        if (!instance->loggedFirstFrame)
        {
            ShimLog("codec: first IV50 frame decoded (%dx%d)", picture.width, picture.height);
            instance->loggedFirstFrame = true;
        }

        return ICERR_OK;
    }

    LRESULT CALLBACK IndeoDriverProc(DWORD_PTR id, HDRVR driver, UINT message,
                                     LPARAM lParam1, LPARAM lParam2)
    {
        switch (message)
        {
        case DRV_LOAD:
        case DRV_ENABLE:
        case DRV_DISABLE:
        case DRV_FREE:
            return 1;

        case DRV_OPEN:
        {
            auto* open = reinterpret_cast<ICOPEN*>(lParam2);
            if (open)
            {
                if (open->fccType && open->fccType != ICTYPE_VIDEO)
                    return 0;
                if (open->fccHandler && !IsIndeo5(open->fccHandler))
                    return 0;
                // We decode only; refuse a compressor open outright so VfW does
                // not offer IV50 as an encoder anywhere in the process.
                if ((open->dwFlags & ICMODE_COMPRESS) &&
                    !(open->dwFlags & ICMODE_DECOMPRESS))
                {
                    return 0;
                }
                open->dwError = ICERR_OK;
            }

            auto* instance = new (std::nothrow) Instance();
            return reinterpret_cast<LRESULT>(instance);
        }

        case DRV_CLOSE:
        {
            Instance* instance = AsInstance(id);
            if (instance)
            {
                if (instance->decoder)
                    IndeoDecoderDestroy(instance->decoder);
                instance->magic = 0;
                delete instance;
            }
            return 1;
        }

        case ICM_GETINFO:
        {
            auto* info = reinterpret_cast<ICINFO*>(lParam1);
            if (!info || lParam2 < static_cast<LPARAM>(sizeof(ICINFO)))
                return 0;

            ZeroMemory(info, sizeof(ICINFO));
            info->dwSize = sizeof(ICINFO);
            info->fccType = ICTYPE_VIDEO;
            info->fccHandler = kFourccIV50;
            info->dwFlags = VIDCF_TEMPORAL;   // inter-frame coded
            info->dwVersion = 0x00050000;
            info->dwVersionICM = ICVERSION;
            wcscpy_s(info->szName, L"IV50");
            wcscpy_s(info->szDescription, L"Indeo Video 5 (Battlezone 1.5 shim)");
            return sizeof(ICINFO);
        }

        case ICM_DECOMPRESS_QUERY:
            return QueryDecompress(reinterpret_cast<const BITMAPINFOHEADER*>(lParam1),
                                   reinterpret_cast<const BITMAPINFOHEADER*>(lParam2));

        case ICM_DECOMPRESS_GET_FORMAT:
        {
            const auto* in = reinterpret_cast<const BITMAPINFOHEADER*>(lParam1);
            auto* out = reinterpret_cast<BITMAPINFOHEADER*>(lParam2);
            if (!in || !IsIndeo5(in->biCompression))
                return ICERR_BADFORMAT;
            if (!out)
                return sizeof(BITMAPINFOHEADER);

            const LONG height = in->biHeight < 0 ? -in->biHeight : in->biHeight;

            ZeroMemory(out, sizeof(BITMAPINFOHEADER));
            out->biSize = sizeof(BITMAPINFOHEADER);
            out->biWidth = in->biWidth;
            out->biHeight = height;
            out->biPlanes = 1;
            out->biBitCount = 24;
            out->biCompression = BI_RGB;
            out->biSizeImage = (((in->biWidth * 3) + 3) & ~3) * height;
            return ICERR_OK;
        }

        case ICM_DECOMPRESS_BEGIN:
            return BeginDecompress(AsInstance(id),
                                   reinterpret_cast<const BITMAPINFOHEADER*>(lParam1),
                                   reinterpret_cast<const BITMAPINFOHEADER*>(lParam2));

        case ICM_DECOMPRESS:
        {
            auto* decompress = reinterpret_cast<ICDECOMPRESS*>(lParam1);
            if (!decompress || !decompress->lpbiInput)
                return ICERR_BADPARAM;
            return Decompress(AsInstance(id), decompress->lpInput,
                              decompress->lpbiInput->biSizeImage,
                              decompress->lpOutput, decompress->dwFlags);
        }

        case ICM_DECOMPRESS_END:
        {
            Instance* instance = AsInstance(id);
            if (instance)
                instance->decoding = false;
            return ICERR_OK;
        }

        // The extended entry points carry source/destination rectangles. We
        // never scale or crop, so anything but a whole frame is declined and
        // the caller falls back to the classic path above.
        case ICM_DECOMPRESSEX_QUERY:
        {
            auto* ex = reinterpret_cast<ICDECOMPRESSEX*>(lParam1);
            if (!ex)
                return ICERR_BADPARAM;
            return QueryDecompress(ex->lpbiSrc, ex->lpbiDst);
        }

        case ICM_DECOMPRESSEX_BEGIN:
        {
            auto* ex = reinterpret_cast<ICDECOMPRESSEX*>(lParam1);
            if (!ex || !ex->lpbiSrc || !ex->lpbiDst)
                return ICERR_BADPARAM;
            if (ex->xSrc || ex->ySrc || ex->xDst || ex->yDst ||
                ex->dxSrc != ex->dxDst || ex->dySrc != ex->dyDst)
            {
                return ICERR_UNSUPPORTED;
            }
            return BeginDecompress(AsInstance(id), ex->lpbiSrc, ex->lpbiDst);
        }

        case ICM_DECOMPRESSEX:
        {
            auto* ex = reinterpret_cast<ICDECOMPRESSEX*>(lParam1);
            if (!ex || !ex->lpbiSrc)
                return ICERR_BADPARAM;
            return Decompress(AsInstance(id), ex->lpSrc, ex->lpbiSrc->biSizeImage,
                              ex->lpDst, ex->dwFlags);
        }

        case ICM_DECOMPRESSEX_END:
        {
            Instance* instance = AsInstance(id);
            if (instance)
                instance->decoding = false;
            return ICERR_OK;
        }

        case ICM_DECOMPRESS_GET_PALETTE:
        case ICM_DECOMPRESS_SET_PALETTE:
            return ICERR_UNSUPPORTED;   // we only ever produce true colour

        default:
            if (message < DRV_USER)
                return DefDriverProc(id, driver, message, lParam1, lParam2);
            return ICERR_UNSUPPORTED;
        }
    }
}

bool InstallVideoCodecShim()
{
    if (g_installed)
        return true;

    // If something on this machine really can decode IV50 -- an original Intel
    // codec, or a codec pack -- leave it alone and stay out of the way.
    if (HIC existing = ICOpen(ICTYPE_VIDEO, kFourccIV50, ICMODE_DECOMPRESS))
    {
        ICClose(existing);
        ShimLog("codec: an IV50 decompressor is already installed; not registering ours");
        return true;
    }

    if (!ICInstall(ICTYPE_VIDEO, kFourccIV50,
                   reinterpret_cast<LPARAM>(&IndeoDriverProc),
                   const_cast<char*>("Indeo Video 5 (Battlezone 1.5 shim)"),
                   ICINSTALL_FUNCTION))
    {
        ShimLog("codec: ICInstall for IV50 failed err=%lu", GetLastError());
        return false;
    }

    // Registration is only meaningful if the ordinary lookup mciavi32 uses can
    // find it, so prove that here rather than discovering it at the first movie.
    HIC probe = ICOpen(ICTYPE_VIDEO, kFourccIV50, ICMODE_DECOMPRESS);
    if (!probe)
    {
        ShimLog("codec: IV50 registered but ICOpen still finds nothing");
        ICRemove(ICTYPE_VIDEO, kFourccIV50, 0);
        return false;
    }

    BITMAPINFOHEADER probeFormat = {};
    probeFormat.biSize = sizeof(probeFormat);
    probeFormat.biWidth = 320;
    probeFormat.biHeight = 240;
    probeFormat.biPlanes = 1;
    probeFormat.biBitCount = 24;
    probeFormat.biCompression = kFourccIV50;

    const DWORD queryResult = ICDecompressQuery(probe, &probeFormat, nullptr);
    ICClose(probe);

    if (queryResult != ICERR_OK)
    {
        ShimLog("codec: IV50 self-test failed (ICDecompressQuery=%lu)", queryResult);
        ICRemove(ICTYPE_VIDEO, kFourccIV50, 0);
        return false;
    }

    g_installed = true;
    ShimLog("codec: in-process Indeo Video 5 decompressor active;"
            " the stock AVI files play as shipped");
    return true;
}

void ShutdownVideoCodecShim()
{
    if (!g_installed)
        return;

    ICRemove(ICTYPE_VIDEO, kFourccIV50, 0);
    g_installed = false;
}
