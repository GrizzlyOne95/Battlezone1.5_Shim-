#include "fullscreen_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace
{
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    Direct3DCreate9Fn g_realDirect3DCreate9 = nullptr;
    CreateDeviceFn g_realCreateDevice = nullptr;
    ResetFn g_realReset = nullptr;

    constexpr size_t kIDirect3D9CreateDeviceIndex = 16;
    constexpr size_t kIDirect3DDevice9ResetIndex = 16;

    struct DisplayModeState
    {
        bool captured = false;
        bool changed = false;
        char deviceName[CCHDEVICENAME] = {};
        DEVMODEA original = {};
    };

    DisplayModeState g_displayMode;

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

    HWND GetDeviceWindow(IDirect3DDevice9* device)
    {
        if (!device)
            return nullptr;

        D3DDEVICE_CREATION_PARAMETERS creation = {};
        if (SUCCEEDED(device->GetCreationParameters(&creation)))
            return creation.hFocusWindow;

        return nullptr;
    }

    bool GetMonitorDevice(HWND hwnd, MONITORINFOEXA& info)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        info = {};
        info.cbSize = sizeof(info);
        return GetMonitorInfoA(monitor, reinterpret_cast<LPMONITORINFO>(&info)) != FALSE;
    }

    void RestoreLegacyDisplayMode()
    {
        if (!g_displayMode.captured)
            return;

        if (g_displayMode.changed)
        {
            LONG result = ChangeDisplaySettingsExA(
                g_displayMode.deviceName,
                &g_displayMode.original,
                nullptr,
                0,
                nullptr);

            ShimLog(
                "display: restoring %s to %lux%lu@%lu result=%ld",
                g_displayMode.deviceName,
                g_displayMode.original.dmPelsWidth,
                g_displayMode.original.dmPelsHeight,
                g_displayMode.original.dmDisplayFrequency,
                result);
        }

        g_displayMode = {};
    }

    bool CaptureOriginalDisplayMode(HWND hwnd)
    {
        MONITORINFOEXA monitor = {};
        if (!GetMonitorDevice(hwnd, monitor))
            return false;

        if (g_displayMode.captured)
        {
            if (_stricmp(g_displayMode.deviceName, monitor.szDevice) == 0)
                return true;

            RestoreLegacyDisplayMode();
        }

        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
            return false;

        g_displayMode.captured = true;
        g_displayMode.changed = false;
        strcpy_s(g_displayMode.deviceName, monitor.szDevice);
        g_displayMode.original = mode;

        ShimLog(
            "display: captured %s desktop mode %lux%lu@%lu",
            g_displayMode.deviceName,
            mode.dmPelsWidth,
            mode.dmPelsHeight,
            mode.dmDisplayFrequency);
        return true;
    }

    bool ApplyLegacyDisplayMode(HWND hwnd, UINT width, UINT height, UINT refresh)
    {
        if (!hwnd || !width || !height)
            return false;

        if (!CaptureOriginalDisplayMode(hwnd))
        {
            ShimLog("display: failed to capture current monitor mode");
            return false;
        }

        DEVMODEA current = {};
        current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsExA(g_displayMode.deviceName, ENUM_CURRENT_SETTINGS, &current, 0))
        {
            ShimLog("display: failed to read current mode for %s", g_displayMode.deviceName);
            return false;
        }

        if (current.dmPelsWidth == width && current.dmPelsHeight == height &&
            (!refresh || current.dmDisplayFrequency == refresh))
        {
            ShimLog(
                "display: %s already at requested legacy mode %ux%u@%lu",
                g_displayMode.deviceName,
                width,
                height,
                current.dmDisplayFrequency);
            return true;
        }

        DEVMODEA desired = current;
        desired.dmSize = sizeof(desired);
        desired.dmPelsWidth = width;
        desired.dmPelsHeight = height;
        desired.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

        if (refresh)
        {
            desired.dmDisplayFrequency = refresh;
            desired.dmFields |= DM_DISPLAYFREQUENCY;
        }

        ShimLog(
            "display: switching %s %lux%lu@%lu -> %ux%u@%u",
            g_displayMode.deviceName,
            current.dmPelsWidth,
            current.dmPelsHeight,
            current.dmDisplayFrequency,
            width,
            height,
            refresh);

        LONG result = ChangeDisplaySettingsExA(
            g_displayMode.deviceName,
            &desired,
            nullptr,
            CDS_FULLSCREEN,
            nullptr);

        if (result != DISP_CHANGE_SUCCESSFUL && refresh)
        {
            ShimLog(
                "display: requested refresh %u rejected result=%ld; retrying %ux%u with driver-selected refresh",
                refresh,
                result,
                width,
                height);

            desired.dmFields &= ~DM_DISPLAYFREQUENCY;
            result = ChangeDisplaySettingsExA(
                g_displayMode.deviceName,
                &desired,
                nullptr,
                CDS_FULLSCREEN,
                nullptr);
        }

        if (result != DISP_CHANGE_SUCCESSFUL)
        {
            ShimLog("display: mode switch failed result=%ld", result);
            return false;
        }

        g_displayMode.changed = true;

        DEVMODEA applied = {};
        applied.dmSize = sizeof(applied);
        if (EnumDisplaySettingsExA(g_displayMode.deviceName, ENUM_CURRENT_SETTINGS, &applied, 0))
        {
            ShimLog(
                "display: active mode is now %lux%lu@%lu",
                applied.dmPelsWidth,
                applied.dmPelsHeight,
                applied.dmDisplayFrequency);
        }

        return true;
    }

    bool MakeWindowBorderless(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(monitor, &info))
            return false;

        LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_POPUP | WS_VISIBLE;
        SetWindowLongPtrA(hwnd, GWL_STYLE, style);

        LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);

        const int width = info.rcMonitor.right - info.rcMonitor.left;
        const int height = info.rcMonitor.bottom - info.rcMonitor.top;

        const BOOL positioned = SetWindowPos(
            hwnd,
            HWND_TOP,
            info.rcMonitor.left,
            info.rcMonitor.top,
            width,
            height,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        ShimLog(
            "display: borderless window positioned at %ld,%ld %dx%d",
            info.rcMonitor.left,
            info.rcMonitor.top,
            width,
            height);

        return positioned != FALSE;
    }

    bool ConvertExclusiveToBorderless(
        HWND focusWindow,
        const D3DPRESENT_PARAMETERS& requested,
        D3DPRESENT_PARAMETERS& converted)
    {
        if (requested.Windowed)
        {
            RestoreLegacyDisplayMode();
            return false;
        }

        converted = requested;
        converted.Windowed = TRUE;
        converted.FullScreen_RefreshRateInHz = 0;

        // We no longer try to stretch only the D3D backbuffer. Battlezone's
        // shell contains legacy Win32/GDI content as well, so temporarily put
        // the monitor itself into the requested legacy mode. That makes the
        // entire shell coordinate space match the screen, just as old
        // exclusive fullscreen did, while D3D remains windowed so the shell
        // can paint correctly.
        if (converted.SwapEffect == D3DSWAPEFFECT_FLIP)
            converted.SwapEffect = D3DSWAPEFFECT_DISCARD;

        HWND deviceWindow = converted.hDeviceWindow ? converted.hDeviceWindow : focusWindow;
        const bool modeChanged = ApplyLegacyDisplayMode(
            deviceWindow,
            requested.BackBufferWidth,
            requested.BackBufferHeight,
            requested.FullScreen_RefreshRateInHz);

        if (!modeChanged)
            ShimLog("fullscreen: display-mode emulation unavailable; continuing borderless at current desktop mode");

        MakeWindowBorderless(deviceWindow);

        ShimLog(
            "fullscreen: converting exclusive request %ux%u fmt=%u refresh=%u -> borderless windowed with display-mode emulation",
            requested.BackBufferWidth,
            requested.BackBufferHeight,
            static_cast<unsigned>(requested.BackBufferFormat),
            requested.FullScreen_RefreshRateInHz);

        return true;
    }

    HRESULT STDMETHODCALLTYPE HookReset(
        IDirect3DDevice9* device,
        D3DPRESENT_PARAMETERS* params)
    {
        if (!g_realReset || !params)
            return g_realReset ? g_realReset(device, params) : D3DERR_INVALIDCALL;

        HWND focusWindow = params->hDeviceWindow;
        if (!focusWindow)
            focusWindow = GetDeviceWindow(device);

        D3DPRESENT_PARAMETERS converted = {};
        if (!ConvertExclusiveToBorderless(focusWindow, *params, converted))
            return g_realReset(device, params);

        HRESULT hr = g_realReset(device, &converted);
        if (SUCCEEDED(hr))
            return hr;

        if (converted.BackBufferFormat != D3DFMT_UNKNOWN)
        {
            D3DPRESENT_PARAMETERS desktopFormat = converted;
            desktopFormat.BackBufferFormat = D3DFMT_UNKNOWN;
            hr = g_realReset(device, &desktopFormat);
            if (SUCCEEDED(hr))
            {
                ShimLog("fullscreen: Reset succeeded after retry with D3DFMT_UNKNOWN");
                return hr;
            }
        }

        ShimLog("fullscreen: borderless Reset failed hr=0x%08lX; restoring desktop and falling back to stock exclusive Reset", hr);
        RestoreLegacyDisplayMode();
        return g_realReset(device, params);
    }

    void HookDeviceReset(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        void** vtable = *reinterpret_cast<void***>(device);
        void* original = reinterpret_cast<void*>(g_realReset);
        if (PatchPointer(&vtable[kIDirect3DDevice9ResetIndex], reinterpret_cast<void*>(&HookReset), &original))
        {
            if (!g_realReset)
                g_realReset = reinterpret_cast<ResetFn>(original);
            ShimLog("fullscreen: IDirect3DDevice9::Reset hook installed");
        }
        else
        {
            ShimLog("fullscreen: failed to hook IDirect3DDevice9::Reset");
        }
    }

    HRESULT STDMETHODCALLTYPE HookCreateDevice(
        IDirect3D9* d3d,
        UINT adapter,
        D3DDEVTYPE deviceType,
        HWND focusWindow,
        DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* params,
        IDirect3DDevice9** outDevice)
    {
        if (!g_realCreateDevice || !params)
            return g_realCreateDevice
                ? g_realCreateDevice(d3d, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice)
                : D3DERR_INVALIDCALL;

        D3DPRESENT_PARAMETERS converted = {};
        const bool wasExclusive = ConvertExclusiveToBorderless(focusWindow, *params, converted);

        HRESULT hr = g_realCreateDevice(
            d3d,
            adapter,
            deviceType,
            focusWindow,
            behaviorFlags,
            wasExclusive ? &converted : params,
            outDevice);

        if (FAILED(hr) && wasExclusive && converted.BackBufferFormat != D3DFMT_UNKNOWN)
        {
            D3DPRESENT_PARAMETERS desktopFormat = converted;
            desktopFormat.BackBufferFormat = D3DFMT_UNKNOWN;
            hr = g_realCreateDevice(
                d3d,
                adapter,
                deviceType,
                focusWindow,
                behaviorFlags,
                &desktopFormat,
                outDevice);

            if (SUCCEEDED(hr))
                ShimLog("fullscreen: CreateDevice succeeded after retry with D3DFMT_UNKNOWN");
        }

        if (FAILED(hr) && wasExclusive)
        {
            ShimLog("fullscreen: borderless CreateDevice failed hr=0x%08lX; restoring desktop and falling back to stock exclusive mode", hr);
            RestoreLegacyDisplayMode();
            hr = g_realCreateDevice(
                d3d,
                adapter,
                deviceType,
                focusWindow,
                behaviorFlags,
                params,
                outDevice);
        }

        if (SUCCEEDED(hr) && outDevice && *outDevice)
            HookDeviceReset(*outDevice);

        return hr;
    }

    void HookCreateDeviceMethod(IDirect3D9* d3d)
    {
        if (!d3d)
            return;

        void** vtable = *reinterpret_cast<void***>(d3d);
        void* original = reinterpret_cast<void*>(g_realCreateDevice);
        if (PatchPointer(&vtable[kIDirect3D9CreateDeviceIndex], reinterpret_cast<void*>(&HookCreateDevice), &original))
        {
            if (!g_realCreateDevice)
                g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(original);
            ShimLog("fullscreen: IDirect3D9::CreateDevice hook installed");
        }
        else
        {
            ShimLog("fullscreen: failed to hook IDirect3D9::CreateDevice");
        }
    }

    IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
    {
        IDirect3D9* d3d = g_realDirect3DCreate9 ? g_realDirect3DCreate9(sdkVersion) : nullptr;
        if (d3d)
            HookCreateDeviceMethod(d3d);
        return d3d;
    }

    bool HookImport(
        HMODULE module,
        const char* importedDll,
        const char* importedName,
        void* replacement,
        void** original)
    {
        if (!module)
            return false;

        const auto base = reinterpret_cast<uint8_t*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;

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

                auto importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + originalThunk->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(importByName->Name), importedName) != 0)
                    continue;

                void** slot = reinterpret_cast<void**>(&firstThunk->u1.Function);
                return PatchPointer(slot, replacement, original);
            }
        }

        return false;
    }
}

bool InstallFullscreenMenuFix()
{
    void* original = nullptr;
    if (!HookImport(
            GetModuleHandleA(nullptr),
            "d3d9.dll",
            "Direct3DCreate9",
            reinterpret_cast<void*>(&HookDirect3DCreate9),
            &original))
    {
        ShimLog("fullscreen: Direct3DCreate9 import was not found/hooked");
        return false;
    }

    g_realDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(original);
    ShimLog("fullscreen: Direct3DCreate9 IAT hook installed");
    return true;
}

void ShutdownFullscreenMenuFix()
{
    RestoreLegacyDisplayMode();
}
