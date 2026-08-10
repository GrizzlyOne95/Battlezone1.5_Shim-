#include "movie_geometry_probe.h"
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

    struct DeviceName
    {
        MCIDEVICEID id = 0;
        char element[MAX_PATH] = {};
    };

    std::array<DeviceName, 16> g_devices = {};

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

    DeviceName* FindDevice(MCIDEVICEID id)
    {
        for (auto& device : g_devices)
        {
            if (device.id == id)
                return &device;
        }
        return nullptr;
    }

    const char* DeviceElement(MCIDEVICEID id)
    {
        if (DeviceName* device = FindDevice(id))
            return device->element[0] ? device->element : "<unknown>";
        return "<unknown>";
    }

    void RememberDevice(MCIDEVICEID id, const char* element)
    {
        if (!id)
            return;

        DeviceName* slot = FindDevice(id);
        if (!slot)
        {
            for (auto& candidate : g_devices)
            {
                if (!candidate.id)
                {
                    slot = &candidate;
                    break;
                }
            }
        }

        if (!slot)
            return;

        slot->id = id;
        slot->element[0] = '\0';
        if (element && *element)
            strncpy_s(slot->element, element, _TRUNCATE);
    }

    void ForgetDevice(MCIDEVICEID id)
    {
        if (DeviceName* device = FindDevice(id))
            *device = {};
    }

    void LogWindowTarget(MCIDEVICEID id, DWORD_PTR flags, DWORD_PTR param)
    {
        if (!param)
            return;

        auto* window = reinterpret_cast<MCI_DGV_WINDOW_PARMSA*>(param);
        HWND hwnd = (flags & MCI_DGV_WINDOW_HWND) ? window->hWnd : nullptr;

        if (!hwnd)
        {
            ShimLog("moviegeom: WINDOW device=%u file=%s flags=%08lX hwnd=<not supplied>",
                    static_cast<unsigned int>(id), DeviceElement(id),
                    static_cast<unsigned long>(flags));
            return;
        }

        RECT wr = {};
        RECT cr = {};
        char className[96] = {};
        const bool valid = IsWindow(hwnd) != FALSE;
        if (valid)
        {
            GetWindowRect(hwnd, &wr);
            GetClientRect(hwnd, &cr);
            GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)));
        }

        ShimLog(
            "moviegeom: WINDOW device=%u file=%s flags=%08lX hwnd=%p valid=%d class=%s parent=%p visible=%d style=%08lX ex=%08lX window=%ld,%ld %ldx%ld client=%ldx%ld",
            static_cast<unsigned int>(id), DeviceElement(id),
            static_cast<unsigned long>(flags), hwnd,
            valid ? 1 : 0,
            className[0] ? className : "?",
            valid ? GetParent(hwnd) : nullptr,
            (valid && IsWindowVisible(hwnd)) ? 1 : 0,
            valid ? static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_STYLE)) : 0ul,
            valid ? static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_EXSTYLE)) : 0ul,
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
            cr.right - cr.left, cr.bottom - cr.top);
    }

    void LogPutRect(MCIDEVICEID id, DWORD_PTR flags, DWORD_PTR param)
    {
        if (!param)
            return;

        auto* put = reinterpret_cast<MCI_DGV_PUT_PARMS*>(param);
        const RECT& rc = put->rc;

        // MCI digital-video RECT semantics are unusual: right/bottom are width
        // and height rather than absolute right/bottom coordinates.
        ShimLog(
            "moviegeom: PUT device=%u file=%s flags=%08lX rect=(x=%ld y=%ld w=%ld h=%ld) rectFlag=%d dest=%d window=%d client=%d frame=%d source=%d video=%d",
            static_cast<unsigned int>(id), DeviceElement(id),
            static_cast<unsigned long>(flags),
            rc.left, rc.top, rc.right, rc.bottom,
            (flags & MCI_DGV_RECT) ? 1 : 0,
            (flags & MCI_DGV_PUT_DESTINATION) ? 1 : 0,
            (flags & MCI_DGV_PUT_WINDOW) ? 1 : 0,
            (flags & MCI_DGV_PUT_CLIENT) ? 1 : 0,
            (flags & MCI_DGV_PUT_FRAME) ? 1 : 0,
            (flags & MCI_DGV_PUT_SOURCE) ? 1 : 0,
            (flags & MCI_DGV_PUT_VIDEO) ? 1 : 0);
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        if (!g_nextMciSendCommandA)
            return MCIERR_UNSUPPORTED_FUNCTION;

        const MCIERROR result = g_nextMciSendCommandA(deviceId, message, flags, param);

        if (message == MCI_OPEN && result == 0 && param)
        {
            auto* open = reinterpret_cast<MCI_OPEN_PARMSA*>(param);
            const char* element = nullptr;
            if ((flags & MCI_OPEN_ELEMENT) && !(flags & MCI_OPEN_ELEMENT_ID))
                element = open->lpstrElementName;
            RememberDevice(open->wDeviceID, element);
            ShimLog("moviegeom: OPEN device=%u file=%s",
                    static_cast<unsigned int>(open->wDeviceID),
                    DeviceElement(open->wDeviceID));
        }
        else if (message == MCI_WINDOW && result == 0)
        {
            LogWindowTarget(deviceId, flags, param);
        }
        else if (message == MCI_PUT && result == 0)
        {
            LogPutRect(deviceId, flags, param);
        }
        else if (message == MCI_CLOSE && result == 0)
        {
            ShimLog("moviegeom: CLOSE device=%u file=%s",
                    static_cast<unsigned int>(deviceId), DeviceElement(deviceId));
            ForgetDevice(deviceId);
        }

        return result;
    }
}

bool InstallMovieGeometryProbe()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !HookImport(exe, "winmm.dll", "mciSendCommandA",
                            reinterpret_cast<void*>(&HookMciSendCommandA),
                            reinterpret_cast<void**>(&g_nextMciSendCommandA)))
    {
        ShimLog("moviegeom: could not install passive MCI geometry probe");
        return false;
    }

    ShimLog("moviegeom: passive MCI WINDOW/PUT geometry probe active");
    return true;
}

void ShutdownMovieGeometryProbe()
{
    g_devices = {};
}
