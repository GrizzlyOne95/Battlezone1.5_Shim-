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
        return SetWindowPos(
                   hwnd,
                   HWND_TOP,
                   info.rcMonitor.left,
                   info.rcMonitor.top,
                   width,
                   height,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW) != FALSE;
    }

    bool ConvertExclusiveToBorderless(
        HWND focusWindow,
        const D3DPRESENT_PARAMETERS& requested,
        D3DPRESENT_PARAMETERS& converted)
    {
        if (requested.Windowed)
            return false;

        converted = requested;
        converted.Windowed = TRUE;
        converted.FullScreen_RefreshRateInHz = 0;

        // D3DSWAPEFFECT_FLIP is an exclusive-fullscreen-era choice. DISCARD is
        // the closest normal D3D9 windowed presentation behavior.
        if (converted.SwapEffect == D3DSWAPEFFECT_FLIP)
            converted.SwapEffect = D3DSWAPEFFECT_DISCARD;

        HWND deviceWindow = converted.hDeviceWindow ? converted.hDeviceWindow : focusWindow;
        MakeWindowBorderless(deviceWindow);

        ShimLog(
            "fullscreen: converting exclusive request %ux%u fmt=%u refresh=%u -> borderless windowed",
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
        if (!focusWindow && device)
        {
            D3DDEVICE_CREATION_PARAMETERS creation = {};
            if (SUCCEEDED(device->GetCreationParameters(&creation)))
                focusWindow = creation.hFocusWindow;
        }

        D3DPRESENT_PARAMETERS converted = {};
        if (!ConvertExclusiveToBorderless(focusWindow, *params, converted))
            return g_realReset(device, params);

        HRESULT hr = g_realReset(device, &converted);
        if (SUCCEEDED(hr))
            return hr;

        // Some older drivers insist on desktop format for windowed reset.
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

        ShimLog("fullscreen: borderless Reset failed hr=0x%08lX; falling back to stock exclusive Reset", hr);
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
            ShimLog("fullscreen: borderless CreateDevice failed hr=0x%08lX; falling back to stock exclusive mode", hr);
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
