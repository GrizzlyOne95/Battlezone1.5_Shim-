#include "movie_paint_fix.h"
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

    constexpr UINT kPaintIntervalMs = 33;
    constexpr UINT_PTR kTimerBase = 0xB150;
    constexpr size_t kMaxMovieSlots = 16;

    struct MovieSlot
    {
        MCIDEVICEID deviceId = 0;
        HWND window = nullptr;
        UINT_PTR timerId = 0;
        bool playing = false;
        bool updateDisabled = false;
        bool updateLogged = false;
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

    MovieSlot* FindSlot(MCIDEVICEID deviceId)
    {
        if (!deviceId)
            return nullptr;

        for (auto& slot : g_slots)
        {
            if (slot.deviceId == deviceId)
                return &slot;
        }
        return nullptr;
    }

    MovieSlot* FindOrAllocateSlot(MCIDEVICEID deviceId)
    {
        if (MovieSlot* existing = FindSlot(deviceId))
            return existing;

        for (auto& slot : g_slots)
        {
            if (!slot.deviceId)
            {
                slot = {};
                slot.deviceId = deviceId;
                return &slot;
            }
        }

        ShimLog("moviepaint: no free slot for MCI device=%u", static_cast<unsigned int>(deviceId));
        return nullptr;
    }

    void StopTimer(MovieSlot& slot)
    {
        slot.playing = false;
        if (slot.timerId && slot.window && IsWindow(slot.window))
            KillTimer(slot.window, slot.timerId);
        slot.timerId = 0;
    }

    void ReleaseSlot(MCIDEVICEID deviceId)
    {
        if (MovieSlot* slot = FindSlot(deviceId))
        {
            StopTimer(*slot);
            *slot = {};
        }
    }

    bool RepaintCurrentFrame(MovieSlot& slot, bool forceLog)
    {
        if (!g_nextMciSendCommandA || slot.updateDisabled ||
            !slot.window || !IsWindow(slot.window))
        {
            return false;
        }

        HDC dc = GetDC(slot.window);
        if (!dc)
            return false;

        MCI_DGV_UPDATE_PARMS update = {};
        update.hDC = dc;

        const MCIERROR result = g_nextMciSendCommandA(
            slot.deviceId,
            MCI_UPDATE,
            MCI_DGV_UPDATE_HDC,
            reinterpret_cast<DWORD_PTR>(&update));

        ReleaseDC(slot.window, dc);

        if (forceLog || !slot.updateLogged || result != 0)
        {
            ShimLog("moviepaint: MCI_UPDATE device=%u hwnd=%p -> err=%lu",
                    static_cast<unsigned int>(slot.deviceId),
                    slot.window,
                    static_cast<unsigned long>(result));
            slot.updateLogged = true;
        }

        if (result != 0)
        {
            slot.updateDisabled = true;
            StopTimer(slot);
            ShimLog("moviepaint: software repaint disabled for device=%u after MCI_UPDATE failure",
                    static_cast<unsigned int>(slot.deviceId));
            return false;
        }

        return true;
    }

    void CALLBACK MoviePaintTimerProc(HWND hwnd, UINT, UINT_PTR timerId, DWORD)
    {
        for (auto& slot : g_slots)
        {
            if (slot.timerId == timerId && slot.window == hwnd && slot.playing)
            {
                RepaintCurrentFrame(slot, false);
                return;
            }
        }
    }

    void DescribeWindow(MCIDEVICEID deviceId, HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
        {
            ShimLog("moviepaint: MCI_WINDOW device=%u supplied invalid hwnd=%p",
                    static_cast<unsigned int>(deviceId), hwnd);
            return;
        }

        RECT windowRect = {};
        RECT clientRect = {};
        GetWindowRect(hwnd, &windowRect);
        GetClientRect(hwnd, &clientRect);

        char className[96] = {};
        GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)));

        ShimLog(
            "moviepaint: device=%u target hwnd=%p class=%s parent=%p style=%08lX ex=%08lX visible=%d window=%ld,%ld %ldx%ld client=%ldx%ld",
            static_cast<unsigned int>(deviceId),
            hwnd,
            className[0] ? className : "?",
            GetParent(hwnd),
            static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_STYLE)),
            static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_EXSTYLE)),
            IsWindowVisible(hwnd) ? 1 : 0,
            windowRect.left,
            windowRect.top,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            clientRect.right - clientRect.left,
            clientRect.bottom - clientRect.top);
    }

    void RememberWindow(MCIDEVICEID deviceId, HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        MovieSlot* slot = FindOrAllocateSlot(deviceId);
        if (!slot)
            return;

        if (slot->window != hwnd)
        {
            StopTimer(*slot);
            slot->window = hwnd;
            slot->updateDisabled = false;
            slot->updateLogged = false;
        }

        DescribeWindow(deviceId, hwnd);

        // MCIAVI can successfully accept/play a movie while its child playback
        // window never becomes visible on modern composition. Keep Battlezone's
        // target child visible and above its owner-drawn siblings; the explicit
        // MCI_UPDATE path below supplies the actual pixels.
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }

    void StartPainting(MCIDEVICEID deviceId)
    {
        MovieSlot* slot = FindSlot(deviceId);
        if (!slot || !slot->window || !IsWindow(slot->window) || slot->updateDisabled)
            return;

        slot->playing = true;
        RepaintCurrentFrame(*slot, true);
        if (slot->updateDisabled)
            return;

        const UINT_PTR requestedId = kTimerBase + static_cast<UINT_PTR>(deviceId);
        slot->timerId = SetTimer(slot->window, requestedId, kPaintIntervalMs, &MoviePaintTimerProc);
        if (!slot->timerId)
        {
            slot->playing = false;
            ShimLog("moviepaint: SetTimer failed for device=%u hwnd=%p err=%lu",
                    static_cast<unsigned int>(deviceId), slot->window, GetLastError());
        }
        else
        {
            ShimLog("moviepaint: software repaint timer active device=%u hwnd=%p interval=%ums",
                    static_cast<unsigned int>(deviceId), slot->window, kPaintIntervalMs);
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

        switch (message)
        {
        case MCI_WINDOW:
            if (param && (flags & MCI_DGV_WINDOW_HWND))
            {
                auto* window = reinterpret_cast<MCI_DGV_WINDOW_PARMSA*>(param);
                RememberWindow(deviceId, window->hWnd);
            }
            break;

        case MCI_PUT:
            if (param && (flags & MCI_DGV_RECT))
            {
                auto* put = reinterpret_cast<MCI_DGV_PUT_PARMS*>(param);
                ShimLog("moviepaint: MCI_PUT device=%u flags=%08lX rect=%ld,%ld %ldx%ld",
                        static_cast<unsigned int>(deviceId),
                        static_cast<unsigned long>(flags),
                        put->rc.left,
                        put->rc.top,
                        put->rc.right,
                        put->rc.bottom);
            }
            if (MovieSlot* slot = FindSlot(deviceId))
                RepaintCurrentFrame(*slot, false);
            break;

        case MCI_PLAY:
            StartPainting(deviceId);
            break;

        case MCI_SEEK:
            if (MovieSlot* slot = FindSlot(deviceId))
                RepaintCurrentFrame(*slot, false);
            break;

        case MCI_STOP:
        case MCI_PAUSE:
            if (MovieSlot* slot = FindSlot(deviceId))
            {
                StopTimer(*slot);
                RepaintCurrentFrame(*slot, false);
            }
            break;

        case MCI_CLOSE:
            ReleaseSlot(deviceId);
            break;

        default:
            break;
        }

        return result;
    }
}

bool InstallLegacyMoviePaintFix()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !HookImport(exe, "winmm.dll", "mciSendCommandA",
                            reinterpret_cast<void*>(&HookMciSendCommandA),
                            reinterpret_cast<void**>(&g_nextMciSendCommandA)))
    {
        ShimLog("moviepaint: could not install MCI software repaint hook");
        return false;
    }

    ShimLog("moviepaint: MCI software repaint fallback active");
    return true;
}

void ShutdownLegacyMoviePaintFix()
{
    for (auto& slot : g_slots)
    {
        if (slot.deviceId)
            StopTimer(slot);
        slot = {};
    }
}
