// codec_selftest -- drives the shim's IV50 decompressor through MCI the same
// way Battlezone does, without needing the game.
//
//     codec_selftest <clip.avi> [out.bmp]
//
// Registers the decompressor, opens the clip as an MCI "AVIVideo" device, plays
// it, then pulls the current frame back with MCI_UPDATE into a memory DC and
// reports what actually landed there. A clip that used to fail MCI_OPEN with
// error 6 should now open, play, and produce a non-black frame.

#include "video_codec_shim.h"

#include <Windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <vfw.h>
#include <cstdint>
#include <cstdio>

namespace
{
    struct Surface
    {
        HDC     dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void*   bits = nullptr;
        int     width = 0;
        int     height = 0;
    };

    bool CreateSurface(Surface& surface, int width, int height)
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;   // top-down, so row 0 is the top
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        surface.dc = CreateCompatibleDC(screen);
        surface.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &surface.bits, nullptr, 0);
        ReleaseDC(nullptr, screen);

        if (!surface.dc || !surface.bitmap || !surface.bits)
            return false;

        surface.previous = SelectObject(surface.dc, surface.bitmap);
        surface.width = width;
        surface.height = height;
        return true;
    }

    void SaveBitmap(const Surface& surface, const char* path)
    {
        const DWORD pixelBytes = static_cast<DWORD>(surface.width) *
                                 static_cast<DWORD>(surface.height) * 4;

        BITMAPFILEHEADER file = {};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + pixelBytes;

        BITMAPINFOHEADER header = {};
        header.biSize = sizeof(header);
        header.biWidth = surface.width;
        header.biHeight = -surface.height;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = pixelBytes;

        FILE* out = fopen(path, "wb");
        if (!out)
            return;

        fwrite(&file, sizeof(file), 1, out);
        fwrite(&header, sizeof(header), 1, out);
        fwrite(surface.bits, 1, pixelBytes, out);
        fclose(out);
        printf("  wrote %s\n", path);
    }

    void PumpFor(DWORD milliseconds)
    {
        const DWORD until = GetTickCount() + milliseconds;
        MSG message;
        while (GetTickCount() < until)
        {
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            Sleep(5);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: codec_selftest <clip.avi> [out.bmp]\n");
        return 2;
    }

    const char* clip = argv[1];
    const char* bmpPath = argc > 2 ? argv[2] : nullptr;

    printf("clip: %s\n", clip);

    HIC before = ICOpen(ICTYPE_VIDEO, mmioFOURCC('I', 'V', '5', '0'), ICMODE_DECOMPRESS);
    printf("  IV50 decompressor before install: %s\n", before ? "present" : "none");
    if (before)
        ICClose(before);

    if (!InstallVideoCodecShim())
    {
        printf("  RESULT: FAIL (could not install the decompressor)\n");
        return 1;
    }

    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "bz15selftest";
    RegisterClassA(&windowClass);

    HWND host = CreateWindowExA(0, "bz15selftest", "codec selftest",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 700, 560,
                                nullptr, nullptr, windowClass.hInstance, nullptr);

    MCI_DGV_OPEN_PARMSA open = {};
    open.lpstrDeviceType = const_cast<LPSTR>("AVIVideo");
    open.lpstrElementName = const_cast<LPSTR>(clip);
    open.hWndParent = host;
    open.dwStyle = WS_CHILD;

    MCIERROR error = mciSendCommandA(0, MCI_OPEN,
                                     MCI_OPEN_TYPE | MCI_OPEN_ELEMENT |
                                         MCI_DGV_OPEN_PARENT | MCI_DGV_OPEN_WS,
                                     reinterpret_cast<DWORD_PTR>(&open));
    if (error)
    {
        char text[256] = {};
        mciGetErrorStringA(error, text, sizeof(text));
        printf("  MCI_OPEN failed: %lu (%s)\n", error, text);
        printf("  RESULT: FAIL\n");
        return 1;
    }
    printf("  MCI_OPEN ok, device %u\n", open.wDeviceID);

    MCI_DGV_WINDOW_PARMSA window = {};
    window.hWnd = host;
    mciSendCommandA(open.wDeviceID, MCI_WINDOW, MCI_DGV_WINDOW_HWND,
                    reinterpret_cast<DWORD_PTR>(&window));

    MCI_DGV_PUT_PARMS put = {};
    put.rc.left = 0;
    put.rc.top = 0;
    put.rc.right = 320;
    put.rc.bottom = 240;
    mciSendCommandA(open.wDeviceID, MCI_PUT, MCI_DGV_RECT | MCI_DGV_PUT_DESTINATION,
                    reinterpret_cast<DWORD_PTR>(&put));

    // Sample from the middle rather than the start: intro.avi and credits.avi
    // both open on a fade from black, so an early frame proves nothing.
    MCI_STATUS_PARMS status = {};
    status.dwItem = MCI_STATUS_LENGTH;
    DWORD_PTR length = 0;
    if (mciSendCommandA(open.wDeviceID, MCI_STATUS, MCI_STATUS_ITEM,
                        reinterpret_cast<DWORD_PTR>(&status)) == 0)
    {
        length = status.dwReturn;
    }

    if (length > 4)
    {
        MCI_SEEK_PARMS seek = {};
        seek.dwTo = static_cast<DWORD>(length / 2);
        mciSendCommandA(open.wDeviceID, MCI_SEEK, MCI_TO,
                        reinterpret_cast<DWORD_PTR>(&seek));
        printf("  seeked to %lu of %lu\n", static_cast<unsigned long>(seek.dwTo),
               static_cast<unsigned long>(length));
    }

    MCI_DGV_PLAY_PARMS play = {};
    error = mciSendCommandA(open.wDeviceID, MCI_PLAY, 0, reinterpret_cast<DWORD_PTR>(&play));
    printf("  MCI_PLAY: %lu\n", error);

    PumpFor(500);

    Surface surface;
    if (!CreateSurface(surface, 640, 480))
    {
        printf("  RESULT: FAIL (no surface)\n");
        return 1;
    }

    MCI_DGV_UPDATE_PARMS update = {};
    update.hDC = surface.dc;
    const MCIERROR updateResult =
        mciSendCommandA(open.wDeviceID, MCI_UPDATE,
                        MCI_DGV_UPDATE_HDC | MCI_DGV_UPDATE_PAINT,
                        reinterpret_cast<DWORD_PTR>(&update));
    printf("  MCI_UPDATE: %lu\n", updateResult);

    // Count what actually arrived. A working decode gives a wide spread of
    // colours; a broken one gives a uniform (usually black) rectangle.
    const auto* pixels = static_cast<const uint32_t*>(surface.bits);
    int nonBlack = 0;
    int distinct = 0;
    bool seen[4096] = {};
    for (int i = 0; i < surface.width * surface.height; ++i)
    {
        const uint32_t pixel = pixels[i] & 0x00FFFFFF;
        if (pixel)
            nonBlack++;
        const int bucket = ((pixel >> 20) & 0xF) << 8 | ((pixel >> 12) & 0xF) << 4 |
                           ((pixel >> 4) & 0xF);
        if (!seen[bucket])
        {
            seen[bucket] = true;
            distinct++;
        }
    }

    printf("  frame: %d non-black pixels, %d distinct colours\n", nonBlack, distinct);

    if (bmpPath)
        SaveBitmap(surface, bmpPath);

    mciSendCommandA(open.wDeviceID, MCI_CLOSE, 0, 0);
    SelectObject(surface.dc, surface.previous);
    DeleteObject(surface.bitmap);
    DeleteDC(surface.dc);
    DestroyWindow(host);
    ShutdownVideoCodecShim();

    if (updateResult != 0)
    {
        printf("  RESULT: FAIL (MCI_UPDATE returned %lu)\n", updateResult);
        return 1;
    }

    // Two distinct colours is the real floor, not an arbitrary one: bzone.avi is
    // a green wireframe title on black and never has more.
    if (nonBlack == 0 || distinct < 2)
    {
        printf("  RESULT: blank (the clip decoded to a flat frame)\n");
        return 1;
    }

    printf("  RESULT: playing\n");
    return 0;
}
