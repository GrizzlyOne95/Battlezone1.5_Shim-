#include "movie_present_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace
{
    using MciSendCommandAFn = MCIERROR (WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);

    MciSendCommandAFn g_nextMciSendCommandA = nullptr;

    constexpr UINT kFrameIntervalMs = 33;
    constexpr UINT_PTR kTimerBase = 0xB250;
    constexpr size_t kMaxMovieSlots = 16;

    struct MovieSlot
    {
        MCIDEVICEID deviceId = 0;
        HWND mciWindow = nullptr;
        RECT destination = {};
        bool haveDestination = false;
        bool playing = false;
        bool disabled = false;
        bool firstPaintLogged = false;
        UINT_PTR timerId = 0;

        HDC memoryDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        void* bits = nullptr;
        int surfaceW = 0;
        int surfaceH = 0;
    };

    std::array<MovieSlot, kMaxMovieSlots> g_slots = {};

    bool PatchPointer(void** slot, void* replacement, void** original)
    {
        if (!slot || !replacement)
            return false;
        if (*slot == replacement)
            return true;

        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        if (original && !*original)
            *original = *slot;

        *slot = replacement;
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        return true;
    }

    bool HookImport(HMODULE module, const char* importedDll, const char* importedName,
                    void* replacement, void** original)
    {
        if (!module)
            return false;

        const auto base = reinterpret_cast<std::uint8_t*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            return false;
        }

        const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!importDir.VirtualAddress || !importDir.Size)
            return false;

        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
        for (; descriptor->Name; ++descriptor)
        {
            const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
            if (_stricmp(dllName, importedDll) != 0)
                continue;

            auto firstThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
            auto originalThunk = descriptor->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk)
                : firstThunk;

            for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk)
            {
                if (IMAGE_SNAP_BY_ORDINAL32(originalThunk->u1.Ordinal))
                    continue;

                auto importByName =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), importedName) != 0)
                    continue;

                void** slot = reinterpret_cast<void**>(&firstThunk->u1.Function);
                return PatchPointer(slot, replacement, original);
            }
        }

        return false;
    }

    MovieSlot* FindSlot(MCIDEVICEID id)
    {
        if (!id)
            return nullptr;
        for (auto& slot : g_slots)
        {
            if (slot.deviceId == id)
                return &slot;
        }
        return nullptr;
    }

    MovieSlot* FindOrAllocateSlot(MCIDEVICEID id)
    {
        if (MovieSlot* slot = FindSlot(id))
            return slot;

        for (auto& slot : g_slots)
        {
            if (!slot.deviceId)
            {
                slot = {};
                slot.deviceId = id;
                return &slot;
            }
        }
        return nullptr;
    }

    void StopTimer(MovieSlot& slot)
    {
        slot.playing = false;
        if (slot.timerId)
            KillTimer(nullptr, slot.timerId);
        slot.timerId = 0;
    }

    void ReleaseSurface(MovieSlot& slot)
    {
        if (slot.memoryDc)
        {
            if (slot.oldBitmap)
                SelectObject(slot.memoryDc, slot.oldBitmap);
            if (slot.dib)
                DeleteObject(slot.dib);
            DeleteDC(slot.memoryDc);
        }

        slot.memoryDc = nullptr;
        slot.dib = nullptr;
        slot.oldBitmap = nullptr;
        slot.bits = nullptr;
        slot.surfaceW = 0;
        slot.surfaceH = 0;
    }

    void ReleaseSlot(MCIDEVICEID id)
    {
        if (MovieSlot* slot = FindSlot(id))
        {
            StopTimer(*slot);
            ReleaseSurface(*slot);
            *slot = {};
        }
    }

    bool EnsureSurface(MovieSlot& slot)
    {
        if (!slot.mciWindow || !IsWindow(slot.mciWindow))
            return false;

        RECT client = {};
        if (!GetClientRect(slot.mciWindow, &client))
            return false;

        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
            return false;

        if (slot.memoryDc && slot.surfaceW == width && slot.surfaceH == height)
            return true;

        ReleaseSurface(slot);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        slot.memoryDc = CreateCompatibleDC(screen);
        slot.dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &slot.bits, nullptr, 0);
        ReleaseDC(nullptr, screen);

        if (!slot.memoryDc || !slot.dib || !slot.bits)
        {
            ReleaseSurface(slot);
            return false;
        }

        slot.oldBitmap = SelectObject(slot.memoryDc, slot.dib);
        slot.surfaceW = width;
        slot.surfaceH = height;
        return true;
    }

    HWND FindVisibleAncestor(HWND hwnd)
    {
        HWND current = hwnd;
        while (current)
        {
            if (IsWindowVisible(current))
                return current;
            current = GetParent(current);
        }
        return nullptr;
    }

    bool FindContentBounds(const MovieSlot& slot, RECT& bounds)
    {
        if (!slot.bits || slot.surfaceW <= 0 || slot.surfaceH <= 0)
            return false;

        const auto* pixels = static_cast<const std::uint32_t*>(slot.bits);
        int minX = slot.surfaceW;
        int minY = slot.surfaceH;
        int maxX = -1;
        int maxY = -1;

        for (int y = 0; y < slot.surfaceH; ++y)
        {
            const auto* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(slot.surfaceW);
            for (int x = 0; x < slot.surfaceW; ++x)
            {
                if ((row[x] & 0x00FFFFFFu) == 0)
                    continue;

                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }

        if (maxX < minX || maxY < minY)
            return false;

        bounds.left = minX;
        bounds.top = minY;
        bounds.right = maxX - minX + 1;
        bounds.bottom = maxY - minY + 1;
        return true;
    }

    bool PaintCurrentFrame(MovieSlot& slot, bool forceLog)
    {
        if (slot.disabled || !slot.mciWindow || !slot.haveDestination ||
            !IsWindow(slot.mciWindow) || !EnsureSurface(slot))
        {
            return false;
        }

        if (slot.bits)
            std::memset(slot.bits, 0, static_cast<size_t>(slot.surfaceW) *
                                      static_cast<size_t>(slot.surfaceH) * 4u);

        MCI_DGV_UPDATE_PARMS update = {};
        update.hDC = slot.memoryDc;

        // MCI_DGV_UPDATE_PAINT is the normal MCIAVI repaint path used by legacy
        // MCI window controls. No HWND state is changed here; MCI paints the
        // current frame only into our private memory DC.
        const MCIERROR updateResult = g_nextMciSendCommandA(
            slot.deviceId,
            MCI_UPDATE,
            MCI_DGV_UPDATE_HDC | MCI_DGV_UPDATE_PAINT,
            reinterpret_cast<DWORD_PTR>(&update));

        if (updateResult != 0)
        {
            if (forceLog || !slot.firstPaintLogged)
            {
                ShimLog("moviepresent: MCI_UPDATE failed device=%u err=%lu",
                        static_cast<unsigned int>(slot.deviceId),
                        static_cast<unsigned long>(updateResult));
                slot.firstPaintLogged = true;
            }
            slot.disabled = true;
            StopTimer(slot);
            return false;
        }

        HWND paintWindow = FindVisibleAncestor(slot.mciWindow);
        if (!paintWindow)
        {
            if (forceLog || !slot.firstPaintLogged)
            {
                ShimLog("moviepresent: no visible ancestor for device=%u hwnd=%p",
                        static_cast<unsigned int>(slot.deviceId), slot.mciWindow);
                slot.firstPaintLogged = true;
            }
            return false;
        }

        POINT targetPoint = { slot.destination.left, slot.destination.top };
        SetLastError(ERROR_SUCCESS);
        const int mapResult = MapWindowPoints(slot.mciWindow, paintWindow, &targetPoint, 1);
        if (!mapResult && GetLastError() != ERROR_SUCCESS)
            return false;

        const int width = static_cast<int>(slot.destination.right);
        const int height = static_cast<int>(slot.destination.bottom);
        if (width <= 0 || height <= 0)
            return false;

        // Most MCIAVI implementations honor the existing MCI_PUT destination
        // while repainting to an HDC. If a modern implementation instead emits
        // the frame at another origin, detect the actual non-black frame bounds
        // and stretch those pixels into Battlezone's requested destination.
        RECT content = {};
        const bool haveContent = FindContentBounds(slot, content);

        int srcX = slot.destination.left;
        int srcY = slot.destination.top;
        int srcW = width;
        int srcH = height;

        if (haveContent)
        {
            const bool destContainsContent =
                content.left < slot.destination.left + width &&
                content.top < slot.destination.top + height &&
                content.left + content.right > slot.destination.left &&
                content.top + content.bottom > slot.destination.top;

            if (!destContainsContent)
            {
                srcX = content.left;
                srcY = content.top;
                srcW = content.right;
                srcH = content.bottom;
            }
        }

        HDC out = GetDC(paintWindow);
        if (!out)
            return false;

        SetStretchBltMode(out, COLORONCOLOR);
        const BOOL blitOk = StretchBlt(
            out,
            targetPoint.x,
            targetPoint.y,
            width,
            height,
            slot.memoryDc,
            srcX,
            srcY,
            srcW,
            srcH,
            SRCCOPY);
        ReleaseDC(paintWindow, out);

        if (forceLog || !slot.firstPaintLogged)
        {
            if (haveContent)
            {
                ShimLog(
                    "moviepresent: device=%u update=success target=%p visibleAncestor=%p dest=%ld,%ld %dx%d mapped=%ld,%ld content=%ld,%ld %ldx%ld src=%d,%d %dx%d blit=%d",
                    static_cast<unsigned int>(slot.deviceId),
                    slot.mciWindow,
                    paintWindow,
                    slot.destination.left,
                    slot.destination.top,
                    width,
                    height,
                    targetPoint.x,
                    targetPoint.y,
                    content.left,
                    content.top,
                    content.right,
                    content.bottom,
                    srcX,
                    srcY,
                    srcW,
                    srcH,
                    blitOk ? 1 : 0);
            }
            else
            {
                ShimLog(
                    "moviepresent: device=%u update=success target=%p visibleAncestor=%p dest=%ld,%ld %dx%d mapped=%ld,%ld content=<black> blit=%d",
                    static_cast<unsigned int>(slot.deviceId),
                    slot.mciWindow,
                    paintWindow,
                    slot.destination.left,
                    slot.destination.top,
                    width,
                    height,
                    targetPoint.x,
                    targetPoint.y,
                    blitOk ? 1 : 0);
            }
            slot.firstPaintLogged = true;
        }

        return blitOk != FALSE;
    }

    void CALLBACK PaintTimerProc(HWND, UINT, UINT_PTR timerId, DWORD)
    {
        for (auto& slot : g_slots)
        {
            if (slot.timerId == timerId && slot.playing)
            {
                PaintCurrentFrame(slot, false);
                return;
            }
        }
    }

    void StartTimer(MovieSlot& slot)
    {
        if (slot.disabled || !slot.mciWindow || !slot.haveDestination)
            return;

        slot.playing = true;
        if (slot.timerId)
            return;

        // Defer the first repaint until after Battlezone's MCI call stack has
        // unwound. The previous crash came from manipulating the playback HWND
        // synchronously from inside MCI_WINDOW; this path never does that.
        const UINT_PTR requested = kTimerBase + static_cast<UINT_PTR>(slot.deviceId);
        slot.timerId = SetTimer(nullptr, requested, kFrameIntervalMs, &PaintTimerProc);
        if (!slot.timerId)
        {
            slot.playing = false;
            ShimLog("moviepresent: SetTimer failed device=%u err=%lu",
                    static_cast<unsigned int>(slot.deviceId), GetLastError());
        }
        else
        {
            ShimLog("moviepresent: deferred offscreen repaint timer active device=%u interval=%ums",
                    static_cast<unsigned int>(slot.deviceId), kFrameIntervalMs);
        }
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        if (!g_nextMciSendCommandA)
            return MCIERR_UNSUPPORTED_FUNCTION;

        if (message == MCI_CLOSE)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StopTimer(*slot);
        }

        const MCIERROR result = g_nextMciSendCommandA(deviceId, message, flags, param);
        if (result != 0)
            return result;

        if (message == MCI_OPEN && param)
        {
            auto* open = reinterpret_cast<MCI_OPEN_PARMSA*>(param);
            if (open->wDeviceID)
                FindOrAllocateSlot(open->wDeviceID);
        }
        else if (message == MCI_WINDOW && param && (flags & MCI_DGV_WINDOW_HWND))
        {
            auto* window = reinterpret_cast<MCI_DGV_WINDOW_PARMSA*>(param);
            if (MovieSlot* slot = FindOrAllocateSlot(deviceId))
            {
                if (slot->mciWindow != window->hWnd)
                {
                    StopTimer(*slot);
                    ReleaseSurface(*slot);
                    slot->mciWindow = window->hWnd;
                    slot->disabled = false;
                    slot->firstPaintLogged = false;
                }
            }
        }
        else if (message == MCI_PUT && param &&
                 (flags & MCI_DGV_RECT) && (flags & MCI_DGV_PUT_DESTINATION))
        {
            auto* put = reinterpret_cast<MCI_DGV_PUT_PARMS*>(param);
            if (MovieSlot* slot = FindOrAllocateSlot(deviceId))
            {
                slot->destination = put->rc;
                slot->haveDestination = true;
            }
        }
        else if (message == MCI_PLAY)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StartTimer(*slot);
        }
        else if (message == MCI_STOP || message == MCI_PAUSE)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StopTimer(*slot);
        }
        else if (message == MCI_CLOSE)
        {
            ReleaseSlot(deviceId);
        }

        return result;
    }
}

bool InstallMoviePresentationFix()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !HookImport(exe, "winmm.dll", "mciSendCommandA",
                            reinterpret_cast<void*>(&HookMciSendCommandA),
                            reinterpret_cast<void**>(&g_nextMciSendCommandA)))
    {
        ShimLog("moviepresent: could not install safe MCI presentation fallback");
        return false;
    }

    ShimLog("moviepresent: safe offscreen MCI_UPDATE presentation fallback active");
    return true;
}

void ShutdownMoviePresentationFix()
{
    for (auto& slot : g_slots)
    {
        if (slot.deviceId)
        {
            StopTimer(slot);
            ReleaseSurface(slot);
        }
        slot = {};
    }
}
