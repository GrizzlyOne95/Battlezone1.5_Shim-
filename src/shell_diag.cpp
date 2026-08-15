// shell_diag.cpp
//
// Passive instrumentation for Battlezone 1.5.2.27's fullscreen shell path.
// This module is deliberately separate from fullscreen_fix.cpp so the original
// exclusive D3D9 + GDI behaviour can be observed with [Fullscreen] Mode=off.
// It does not rewrite presentation parameters, window styles, display modes or
// SetDialogBoxMode arguments. It only records what the game and Windows do.

#include "shell_diag.h"
#include "shim_log.h"

#include <Windows.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <cstdint>
#include <cstring>

namespace
{
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using SetDialogBoxModeFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, BOOL);
    using BeginPaintFn = HDC(WINAPI*)(HWND, LPPAINTSTRUCT);

    constexpr size_t kIDirect3D9CreateDeviceIndex = 16;
    constexpr size_t kIDirect3DDevice9PresentIndex = 17;
    constexpr size_t kIDirect3DDevice9SetDialogBoxModeIndex = 20;

    Direct3DCreate9Fn g_realDirect3DCreate9 = nullptr;
    CreateDeviceFn g_realCreateDevice = nullptr;
    PresentFn g_realPresent = nullptr;
    SetDialogBoxModeFn g_realSetDialogBoxMode = nullptr;
    BeginPaintFn g_realBeginPaint = nullptr;

    bool g_enabled = false;
    bool g_dialogModeRequested = false;
    HWND g_gameWindow = nullptr;
    DWORD g_dialogStartTick = 0;
    unsigned long g_presentsInDialogMode = 0;
    unsigned long g_paintsInDialogMode = 0;
    bool g_loggedWindowTreeOnPresent = false;
    bool g_loggedWindowTreeOnPaint = false;

    void BuildIniPath(char (&path)[MAX_PATH])
    {
        path[0] = '\0';
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return;

        char* slash = strrchr(path, '\\');
        if (!slash)
        {
            path[0] = '\0';
            return;
        }

        slash[1] = '\0';
        strcat_s(path, "bz15_shim.ini");
    }

    bool ReadEnabledSetting()
    {
        char ini[MAX_PATH] = {};
        BuildIniPath(ini);
        if (!ini[0])
            return false;

        char value[32] = {};
        GetPrivateProfileStringA(
            "Fullscreen", "DiagnoseShell", "off", value, sizeof(value), ini);

        return _stricmp(value, "on") == 0 ||
               _stricmp(value, "true") == 0 ||
               _stricmp(value, "yes") == 0 ||
               strcmp(value, "1") == 0;
    }

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
                if (strcmp(reinterpret_cast<const char*>(importByName->Name), importedName) != 0)
                    continue;

                void** slot = reinterpret_cast<void**>(&firstThunk->u1.Function);
                return PatchPointer(slot, replacement, original);
            }
        }

        return false;
    }

    bool IsTrackedWindow(HWND hwnd)
    {
        if (!g_gameWindow || !IsWindow(g_gameWindow) || !hwnd || !IsWindow(hwnd))
            return false;
        return hwnd == g_gameWindow || IsChild(g_gameWindow, hwnd) != FALSE;
    }

    void LogOperatingSystem()
    {
        struct RtlOsVersionInfo
        {
            ULONG dwOSVersionInfoSize;
            ULONG dwMajorVersion;
            ULONG dwMinorVersion;
            ULONG dwBuildNumber;
            ULONG dwPlatformId;
            WCHAR szCSDVersion[128];
        };

        using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfo*);

        ULONG major = 0;
        ULONG minor = 0;
        ULONG build = 0;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll)
        {
            auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                GetProcAddress(ntdll, "RtlGetVersion"));
            if (rtlGetVersion)
            {
                RtlOsVersionInfo info = {};
                info.dwOSVersionInfoSize = sizeof(info);
                if (rtlGetVersion(&info) == 0)
                {
                    major = info.dwMajorVersion;
                    minor = info.dwMinorVersion;
                    build = info.dwBuildNumber;
                }
            }
        }

        BOOL composition = FALSE;
        const HRESULT dwmHr = DwmIsCompositionEnabled(&composition);
        ShimLog(
            "shell-diag: OS=%lu.%lu build=%lu DWM-composition=%d (hr=0x%08lX)",
            major,
            minor,
            build,
            composition ? 1 : 0,
            static_cast<unsigned long>(dwmHr));
    }

    void LogPresentParameters(const char* stage, const D3DPRESENT_PARAMETERS& pp)
    {
        ShimLog(
            "shell-diag: [%s] pp windowed=%d bb=%ux%u fmt=%u count=%u swap=%u "
            "ms=%u msq=%lu flags=0x%08lX refresh=%u interval=0x%08X hwnd=%p",
            stage,
            pp.Windowed ? 1 : 0,
            pp.BackBufferWidth,
            pp.BackBufferHeight,
            static_cast<unsigned>(pp.BackBufferFormat),
            pp.BackBufferCount,
            static_cast<unsigned>(pp.SwapEffect),
            static_cast<unsigned>(pp.MultiSampleType),
            pp.MultiSampleQuality,
            pp.Flags,
            pp.FullScreen_RefreshRateInHz,
            pp.PresentationInterval,
            pp.hDeviceWindow);
    }

    void LogDeviceState(IDirect3DDevice9* device, const char* stage)
    {
        if (!device)
            return;

        D3DDEVICE_CREATION_PARAMETERS cp = {};
        const HRESULT cpHr = device->GetCreationParameters(&cp);
        if (SUCCEEDED(cpHr))
        {
            ShimLog(
                "shell-diag: [%s] device adapter=%u type=%u focus=%p behavior=0x%08lX",
                stage,
                cp.AdapterOrdinal,
                static_cast<unsigned>(cp.DeviceType),
                cp.hFocusWindow,
                cp.BehaviorFlags);
        }
        else
        {
            ShimLog("shell-diag: [%s] GetCreationParameters failed hr=0x%08lX",
                    stage, static_cast<unsigned long>(cpHr));
        }

        IDirect3DSwapChain9* swap = nullptr;
        const HRESULT swapHr = device->GetSwapChain(0, &swap);
        if (SUCCEEDED(swapHr) && swap)
        {
            D3DPRESENT_PARAMETERS pp = {};
            const HRESULT ppHr = swap->GetPresentParameters(&pp);
            if (SUCCEEDED(ppHr))
                LogPresentParameters(stage, pp);
            else
                ShimLog("shell-diag: [%s] GetPresentParameters failed hr=0x%08lX",
                        stage, static_cast<unsigned long>(ppHr));
            swap->Release();
        }
        else
        {
            ShimLog("shell-diag: [%s] GetSwapChain(0) failed hr=0x%08lX",
                    stage, static_cast<unsigned long>(swapHr));
        }

        D3DDISPLAYMODE mode = {};
        const HRESULT modeHr = device->GetDisplayMode(0, &mode);
        if (SUCCEEDED(modeHr))
        {
            ShimLog(
                "shell-diag: [%s] display=%ux%u@%u fmt=%u",
                stage,
                mode.Width,
                mode.Height,
                mode.RefreshRate,
                static_cast<unsigned>(mode.Format));
        }
    }

    struct WindowEnumContext
    {
        const char* stage;
        unsigned count;
    };

    BOOL CALLBACK LogChildWindow(HWND hwnd, LPARAM lParam)
    {
        auto* context = reinterpret_cast<WindowEnumContext*>(lParam);
        if (!context || context->count >= 32)
            return FALSE;

        ++context->count;

        char className[128] = {};
        char title[128] = {};
        GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)));
        GetWindowTextA(hwnd, title, static_cast<int>(sizeof(title)));

        RECT wr = {};
        RECT cr = {};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);

        COLORREF center = CLR_INVALID;
        COLORREF quarter = CLR_INVALID;
        COLORREF threeQuarter = CLR_INVALID;
        const LONG clientW = cr.right - cr.left;
        const LONG clientH = cr.bottom - cr.top;
        if (clientW > 2 && clientH > 2)
        {
            HDC dc = GetDC(hwnd);
            if (dc)
            {
                center = GetPixel(dc, clientW / 2, clientH / 2);
                quarter = GetPixel(dc, clientW / 4, clientH / 4);
                threeQuarter = GetPixel(dc, (clientW * 3) / 4, (clientH * 3) / 4);
                ReleaseDC(hwnd, dc);
            }
        }

        ShimLog(
            "shell-diag: [%s] child#%u hwnd=%p parent=%p class='%s' title='%s' "
            "visible=%d enabled=%d rect=%ld,%ld %ldx%ld client=%ldx%ld "
            "style=0x%08lX ex=0x%08lX dc=%08lX/%08lX/%08lX",
            context->stage,
            context->count,
            hwnd,
            GetParent(hwnd),
            className,
            title,
            IsWindowVisible(hwnd) ? 1 : 0,
            IsWindowEnabled(hwnd) ? 1 : 0,
            wr.left,
            wr.top,
            wr.right - wr.left,
            wr.bottom - wr.top,
            clientW,
            clientH,
            static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_STYLE)),
            static_cast<unsigned long>(GetWindowLongA(hwnd, GWL_EXSTYLE)),
            static_cast<unsigned long>(center),
            static_cast<unsigned long>(quarter),
            static_cast<unsigned long>(threeQuarter));

        return TRUE;
    }

    void LogWindowTree(const char* stage)
    {
        if (!g_gameWindow || !IsWindow(g_gameWindow))
        {
            ShimLog("shell-diag: [%s] no valid game window", stage);
            return;
        }

        char className[128] = {};
        char title[128] = {};
        GetClassNameA(g_gameWindow, className, static_cast<int>(sizeof(className)));
        GetWindowTextA(g_gameWindow, title, static_cast<int>(sizeof(title)));

        RECT wr = {};
        RECT cr = {};
        GetWindowRect(g_gameWindow, &wr);
        GetClientRect(g_gameWindow, &cr);

        ShimLog(
            "shell-diag: [%s] game hwnd=%p class='%s' title='%s' visible=%d "
            "rect=%ld,%ld %ldx%ld client=%ldx%ld style=0x%08lX ex=0x%08lX",
            stage,
            g_gameWindow,
            className,
            title,
            IsWindowVisible(g_gameWindow) ? 1 : 0,
            wr.left,
            wr.top,
            wr.right - wr.left,
            wr.bottom - wr.top,
            cr.right - cr.left,
            cr.bottom - cr.top,
            static_cast<unsigned long>(GetWindowLongA(g_gameWindow, GWL_STYLE)),
            static_cast<unsigned long>(GetWindowLongA(g_gameWindow, GWL_EXSTYLE)));

        WindowEnumContext context = { stage, 0 };
        EnumChildWindows(g_gameWindow, &LogChildWindow, reinterpret_cast<LPARAM>(&context));
        ShimLog("shell-diag: [%s] enumerated %u child/descendant windows", stage, context.count);
    }

    HDC WINAPI HookBeginPaint(HWND hwnd, LPPAINTSTRUCT paint)
    {
        HDC dc = g_realBeginPaint ? g_realBeginPaint(hwnd, paint) : nullptr;

        if (g_enabled && g_dialogModeRequested && IsTrackedWindow(hwnd) && paint)
        {
            ++g_paintsInDialogMode;
            if (g_paintsInDialogMode <= 64 || (g_paintsInDialogMode % 120) == 0)
            {
                char className[128] = {};
                GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)));
                ShimLog(
                    "shell-diag: BeginPaint#%lu hwnd=%p class='%s' rc=%ld,%ld..%ld,%ld erase=%d hdc=%p",
                    g_paintsInDialogMode,
                    hwnd,
                    className,
                    paint->rcPaint.left,
                    paint->rcPaint.top,
                    paint->rcPaint.right,
                    paint->rcPaint.bottom,
                    paint->fErase ? 1 : 0,
                    dc);
            }

            if (!g_loggedWindowTreeOnPaint)
            {
                g_loggedWindowTreeOnPaint = true;
                LogWindowTree("first-shell-BeginPaint");
            }
        }

        return dc;
    }

    HRESULT STDMETHODCALLTYPE HookPresent(
        IDirect3DDevice9* device,
        const RECT* sourceRect,
        const RECT* destRect,
        HWND destWindowOverride,
        const RGNDATA* dirtyRegion)
    {
        if (!g_realPresent)
            return D3DERR_INVALIDCALL;

        const HRESULT hr = g_realPresent(
            device, sourceRect, destRect, destWindowOverride, dirtyRegion);

        if (g_enabled && g_dialogModeRequested)
        {
            ++g_presentsInDialogMode;
            const DWORD elapsed = GetTickCount() - g_dialogStartTick;
            if (g_presentsInDialogMode <= 16 || (g_presentsInDialogMode % 120) == 0)
            {
                ShimLog(
                    "shell-diag: Present#%lu +%lums hr=0x%08lX src=%s dst=%s override=%p dirty=%s",
                    g_presentsInDialogMode,
                    elapsed,
                    static_cast<unsigned long>(hr),
                    sourceRect ? "set" : "null",
                    destRect ? "set" : "null",
                    destWindowOverride,
                    dirtyRegion ? "set" : "null");
            }

            if (!g_loggedWindowTreeOnPresent)
            {
                g_loggedWindowTreeOnPresent = true;
                LogDeviceState(device, "first-shell-Present");
                LogWindowTree("first-shell-Present");
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE HookSetDialogBoxMode(IDirect3DDevice9* device, BOOL enableDialogs)
    {
        if (!g_realSetDialogBoxMode)
            return D3DERR_INVALIDCALL;

        const bool enabling = enableDialogs != FALSE;
        if (enabling)
        {
            g_dialogModeRequested = true;
            g_dialogStartTick = GetTickCount();
            g_presentsInDialogMode = 0;
            g_paintsInDialogMode = 0;
            g_loggedWindowTreeOnPresent = false;
            g_loggedWindowTreeOnPaint = false;
        }

        ShimLog("shell-diag: SetDialogBoxMode(%d) entering", enabling ? 1 : 0);
        LogDeviceState(device, enabling ? "before-dialog-enable" : "before-dialog-disable");
        LogWindowTree(enabling ? "before-dialog-enable" : "before-dialog-disable");

        const HRESULT hr = g_realSetDialogBoxMode(device, enableDialogs);

        ShimLog(
            "shell-diag: SetDialogBoxMode(%d) -> hr=0x%08lX",
            enabling ? 1 : 0,
            static_cast<unsigned long>(hr));
        LogDeviceState(device, enabling ? "after-dialog-enable" : "after-dialog-disable");
        LogWindowTree(enabling ? "after-dialog-enable" : "after-dialog-disable");

        if (!enabling)
        {
            ShimLog(
                "shell-diag: shell interval summary presents=%lu paints=%lu duration=%lums",
                g_presentsInDialogMode,
                g_paintsInDialogMode,
                GetTickCount() - g_dialogStartTick);
            g_dialogModeRequested = false;
        }

        return hr;
    }

    void HookDeviceMethods(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        void** vtable = *reinterpret_cast<void***>(device);

        void* presentOriginal = reinterpret_cast<void*>(g_realPresent);
        if (PatchPointer(
                &vtable[kIDirect3DDevice9PresentIndex],
                reinterpret_cast<void*>(&HookPresent),
                &presentOriginal))
        {
            if (!g_realPresent)
            {
                g_realPresent = reinterpret_cast<PresentFn>(presentOriginal);
                ShimLog("shell-diag: IDirect3DDevice9::Present hook installed");
            }
        }
        else
        {
            ShimLog("shell-diag: failed to hook IDirect3DDevice9::Present");
        }

        void* dialogOriginal = reinterpret_cast<void*>(g_realSetDialogBoxMode);
        if (PatchPointer(
                &vtable[kIDirect3DDevice9SetDialogBoxModeIndex],
                reinterpret_cast<void*>(&HookSetDialogBoxMode),
                &dialogOriginal))
        {
            if (!g_realSetDialogBoxMode)
            {
                g_realSetDialogBoxMode = reinterpret_cast<SetDialogBoxModeFn>(dialogOriginal);
                ShimLog("shell-diag: IDirect3DDevice9::SetDialogBoxMode hook installed");
            }
        }
        else
        {
            ShimLog("shell-diag: failed to hook IDirect3DDevice9::SetDialogBoxMode");
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
        if (!g_realCreateDevice)
            return D3DERR_INVALIDCALL;

        if (params)
        {
            g_gameWindow = params->hDeviceWindow ? params->hDeviceWindow : focusWindow;
            LogPresentParameters("CreateDevice-request", *params);
            ShimLog(
                "shell-diag: CreateDevice adapter=%u type=%u focus=%p behavior=0x%08lX gameWindow=%p",
                adapter,
                static_cast<unsigned>(deviceType),
                focusWindow,
                behaviorFlags,
                g_gameWindow);
        }

        const HRESULT hr = g_realCreateDevice(
            d3d, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice);

        ShimLog("shell-diag: CreateDevice -> hr=0x%08lX device=%p",
                static_cast<unsigned long>(hr),
                (outDevice && SUCCEEDED(hr)) ? *outDevice : nullptr);

        if (SUCCEEDED(hr) && outDevice && *outDevice)
        {
            HookDeviceMethods(*outDevice);
            LogDeviceState(*outDevice, "CreateDevice-granted");
            LogWindowTree("CreateDevice-granted");
        }

        return hr;
    }

    void HookCreateDeviceMethod(IDirect3D9* d3d)
    {
        if (!d3d)
            return;

        void** vtable = *reinterpret_cast<void***>(d3d);
        void* original = reinterpret_cast<void*>(g_realCreateDevice);
        if (PatchPointer(
                &vtable[kIDirect3D9CreateDeviceIndex],
                reinterpret_cast<void*>(&HookCreateDevice),
                &original))
        {
            if (!g_realCreateDevice)
            {
                g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(original);
                ShimLog("shell-diag: IDirect3D9::CreateDevice hook installed");
            }
        }
        else
        {
            ShimLog("shell-diag: failed to hook IDirect3D9::CreateDevice");
        }
    }

    IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
    {
        IDirect3D9* d3d = g_realDirect3DCreate9 ? g_realDirect3DCreate9(sdkVersion) : nullptr;
        if (d3d)
            HookCreateDeviceMethod(d3d);
        return d3d;
    }
}

bool InstallShellPresentationDiagnostics()
{
    if (!ReadEnabledSetting())
        return false;

    g_enabled = true;
    LogOperatingSystem();

    HMODULE exe = GetModuleHandleA(nullptr);

    void* beginPaintOriginal = nullptr;
    if (HookImport(
            exe,
            "user32.dll",
            "BeginPaint",
            reinterpret_cast<void*>(&HookBeginPaint),
            &beginPaintOriginal) && beginPaintOriginal)
    {
        g_realBeginPaint = reinterpret_cast<BeginPaintFn>(beginPaintOriginal);
        ShimLog("shell-diag: BeginPaint IAT hook installed");
    }
    else
    {
        // BeginPaint is useful corroboration, not required for D3D diagnostics.
        ShimLog("shell-diag: BeginPaint import hook unavailable; continuing without paint events");
    }

    void* original = nullptr;
    if (!HookImport(
            exe,
            "d3d9.dll",
            "Direct3DCreate9",
            reinterpret_cast<void*>(&HookDirect3DCreate9),
            &original) || !original)
    {
        ShimLog("shell-diag: Direct3DCreate9 import was not found/hooked");
        g_enabled = false;
        return false;
    }

    g_realDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(original);
    ShimLog("shell-diag: Direct3DCreate9 IAT hook installed; diagnostics are passive");
    return true;
}

void ShutdownShellPresentationDiagnostics()
{
    if (g_enabled && g_dialogModeRequested)
    {
        ShimLog(
            "shell-diag: shutdown summary presents=%lu paints=%lu duration=%lums",
            g_presentsInDialogMode,
            g_paintsInDialogMode,
            GetTickCount() - g_dialogStartTick);
    }

    g_enabled = false;
    g_dialogModeRequested = false;
}
