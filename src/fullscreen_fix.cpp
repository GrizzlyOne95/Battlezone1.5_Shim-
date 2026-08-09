// fullscreen_fix.cpp
//
// Battlezone 1.5.2.27 renders its shell (main menu, options, mission select)
// with GDI drawn straight into the Direct3D 9 back buffer: D3D_Get_DC calls
// IDirect3DSurface9::GetDC on lpBackBuffer, the shell paints text and bitmaps
// through that HDC, and D3D_Flip releases it before Present. To make that legal
// in exclusive fullscreen the game calls IDirect3DDevice9::SetDialogBoxMode(TRUE)
// whenever ResolutionMode == 0 (the 640x480 shell mode).
//
// GDI-over-exclusive-fullscreen is effectively dead on modern WDDM drivers, so
// the shell paints nothing and the player has to Alt+Enter into a window to see
// the menu. Gameplay is unaffected because it is pure D3D with no GDI.
//
// The fix: never let the game hold an exclusive swap chain. Every exclusive
// request becomes a windowed one on a borderless window covering the monitor.
// GDI then works exactly as it does in the windowed mode the player already
// falls back to, and D3D9 upscales the 640x480 shell back buffer to the screen.
//
// Two things have to be defended after that:
//
//   1. D3D_Change_Mode_Ex re-applies its own window style and size immediately
//      after D3DAppIResetDevice returns. Because it sees Client_Width (640) <
//      Screen_Width it picks a captioned 640x480 window and undoes the borderless
//      layout. SetWindowPos / SetWindowLongA / MoveWindow are hooked to pin it.
//
//   2. ProcessMouseMessages derives motion from WM_MOUSEMOVE client coordinates
//      against Device.Client_Width/Height, which stay at the logical mode size
//      (640x480). With a 3840x2160 client those coordinates are meaningless, so
//      mouse messages are scaled back into logical space and GetClientRect /
//      ClientToScreen are made to agree.
//
// Settings live in bz15_shim.ini next to bzone.exe:
//
//   [Fullscreen]
//   Mode=borderless   ; borderless (default) | off  -> leave the game stock
//   Scaling=aspect    ; aspect (default) | stretch | center

#include "fullscreen_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <windowsx.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace
{
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

    using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    using SetWindowLongAFn = LONG(WINAPI*)(HWND, int, LONG);
    using MoveWindowFn = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
    using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);
    using ClientToScreenFn = BOOL(WINAPI*)(HWND, LPPOINT);
    using GetMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
    using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

    constexpr size_t kIDirect3D9CreateDeviceIndex = 16;
    constexpr size_t kIDirect3DDevice9ResetIndex = 16;
    constexpr size_t kIDirect3DDevice9PresentIndex = 17;

    Direct3DCreate9Fn g_realDirect3DCreate9 = nullptr;
    CreateDeviceFn g_realCreateDevice = nullptr;
    ResetFn g_realReset = nullptr;
    PresentFn g_realPresent = nullptr;

    SetWindowPosFn g_realSetWindowPos = nullptr;
    SetWindowLongAFn g_realSetWindowLongA = nullptr;
    MoveWindowFn g_realMoveWindow = nullptr;
    GetClientRectFn g_realGetClientRect = nullptr;
    ClientToScreenFn g_realClientToScreen = nullptr;
    GetMessageAFn g_realGetMessageA = nullptr;
    PeekMessageAFn g_realPeekMessageA = nullptr;

    enum class Scaling
    {
        Aspect,
        Stretch,
        Center,
    };

    bool g_enabled = true;
    Scaling g_scaling = Scaling::Aspect;

    // Set while an exclusive request is being served by a borderless window.
    bool g_active = false;
    HWND g_gameWindow = nullptr;

    // The back buffer size the game asked for; also the coordinate space the
    // game's shell layout, hit testing and mouse maths all live in.
    LONG g_logicalW = 0;
    LONG g_logicalH = 0;

    // Borderless window rect in screen coordinates, and the sub-rectangle of the
    // client area that the back buffer is presented into.
    RECT g_windowRect = {};
    RECT g_destRect = {};

    // True when the destination is not the whole client area, which is the only
    // case where Present needs an explicit rectangle (and therefore COPY).
    bool g_needsPresentRect = false;

    // True when logical space and client space differ at all, i.e. input needs
    // remapping. False for a native-resolution mission, which stays a passthrough.
    bool g_needsInputMapping = false;

    LONG ClientWidth() { return g_windowRect.right - g_windowRect.left; }
    LONG ClientHeight() { return g_windowRect.bottom - g_windowRect.top; }

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

    void LoadSettings()
    {
        char ini[MAX_PATH] = {};
        BuildIniPath(ini);
        if (!ini[0])
            return;

        char mode[32] = {};
        GetPrivateProfileStringA("Fullscreen", "Mode", "borderless", mode, sizeof(mode), ini);
        g_enabled = _stricmp(mode, "off") != 0 && _stricmp(mode, "exclusive") != 0;

        char scaling[32] = {};
        GetPrivateProfileStringA("Fullscreen", "Scaling", "aspect", scaling, sizeof(scaling), ini);
        if (_stricmp(scaling, "stretch") == 0)
            g_scaling = Scaling::Stretch;
        else if (_stricmp(scaling, "center") == 0)
            g_scaling = Scaling::Center;
        else
            g_scaling = Scaling::Aspect;

        ShimLog(
            "fullscreen: settings mode=%s scaling=%s (%s)",
            g_enabled ? "borderless" : "off",
            scaling,
            ini);
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

    HWND ResolveDeviceWindow(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* params)
    {
        if (params && params->hDeviceWindow)
            return params->hDeviceWindow;

        if (device)
        {
            D3DDEVICE_CREATION_PARAMETERS creation = {};
            if (SUCCEEDED(device->GetCreationParameters(&creation)) && creation.hFocusWindow)
                return creation.hFocusWindow;
        }

        return g_gameWindow;
    }

    bool GetMonitorRect(HWND hwnd, RECT& out)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(monitor, &info))
            return false;

        out = info.rcMonitor;
        return true;
    }

    // Where the logical image lands inside the borderless client area.
    RECT ComputeDestRect(LONG clientW, LONG clientH)
    {
        RECT dest = { 0, 0, clientW, clientH };

        if (g_scaling == Scaling::Stretch || g_logicalW <= 0 || g_logicalH <= 0)
            return dest;

        LONG width = clientW;
        LONG height = clientH;

        if (g_scaling == Scaling::Center)
        {
            width = g_logicalW;
            height = g_logicalH;
        }
        else
        {
            // Aspect: fit the logical aspect ratio inside the client area.
            const long long byWidth = static_cast<long long>(clientW) * g_logicalH;
            const long long byHeight = static_cast<long long>(clientH) * g_logicalW;

            if (byWidth > byHeight)
            {
                height = clientH;
                width = static_cast<LONG>(byHeight / g_logicalH);
            }
            else
            {
                width = clientW;
                height = static_cast<LONG>(byWidth / g_logicalW);
            }
        }

        if (width > clientW)
            width = clientW;
        if (height > clientH)
            height = clientH;

        dest.left = (clientW - width) / 2;
        dest.top = (clientH - height) / 2;
        dest.right = dest.left + width;
        dest.bottom = dest.top + height;
        return dest;
    }

    void PinBorderlessWindow(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd) || !g_realSetWindowPos || !g_realSetWindowLongA)
            return;

        LONG style = GetWindowLongA(hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                   WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= WS_POPUP | WS_VISIBLE;
        g_realSetWindowLongA(hwnd, GWL_STYLE, style);

        LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        g_realSetWindowLongA(hwnd, GWL_EXSTYLE, exStyle);

        // Black background so the pillarbox bars are not left showing whatever
        // was on screen before. The game never paints outside its dest rect.
        SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND,
                         reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));

        g_realSetWindowPos(
            hwnd,
            HWND_TOP,
            g_windowRect.left,
            g_windowRect.top,
            ClientWidth(),
            ClientHeight(),
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }

    // Rebuild every derived rectangle for a new logical size. Returns false if
    // the monitor geometry could not be read, in which case we stay stock.
    bool ActivateBorderless(HWND hwnd, UINT backBufferWidth, UINT backBufferHeight)
    {
        RECT monitor = {};
        if (!GetMonitorRect(hwnd, monitor))
            return false;

        g_gameWindow = hwnd;
        g_windowRect = monitor;
        g_logicalW = static_cast<LONG>(backBufferWidth);
        g_logicalH = static_cast<LONG>(backBufferHeight);

        const LONG clientW = ClientWidth();
        const LONG clientH = ClientHeight();
        if (clientW <= 0 || clientH <= 0 || g_logicalW <= 0 || g_logicalH <= 0)
            return false;

        g_destRect = ComputeDestRect(clientW, clientH);

        const bool destIsWholeClient =
            g_destRect.left == 0 && g_destRect.top == 0 &&
            g_destRect.right == clientW && g_destRect.bottom == clientH;

        g_needsPresentRect = !destIsWholeClient;
        g_needsInputMapping = !(destIsWholeClient && g_logicalW == clientW && g_logicalH == clientH);

        g_active = true;
        PinBorderlessWindow(hwnd);

        ShimLog(
            "fullscreen: borderless %ldx%ld, logical %ldx%ld -> dest %ld,%ld %ldx%ld "
            "(presentRect=%d inputMap=%d)",
            clientW,
            clientH,
            g_logicalW,
            g_logicalH,
            g_destRect.left,
            g_destRect.top,
            g_destRect.right - g_destRect.left,
            g_destRect.bottom - g_destRect.top,
            g_needsPresentRect ? 1 : 0,
            g_needsInputMapping ? 1 : 0);

        return true;
    }

    void DeactivateBorderless(const char* reason)
    {
        if (!g_active)
            return;

        g_active = false;
        g_needsPresentRect = false;
        g_needsInputMapping = false;
        ShimLog("fullscreen: borderless conversion disengaged (%s)", reason);
    }

    // Rewrite an exclusive request into a borderless windowed one. Returns true
    // when the conversion was applied and `converted` should be used instead.
    bool ConvertExclusiveToBorderless(
        HWND deviceWindow,
        const D3DPRESENT_PARAMETERS& requested,
        D3DPRESENT_PARAMETERS& converted)
    {
        if (!g_enabled)
            return false;

        if (requested.Windowed)
        {
            // The player asked for a real window (Alt+Enter). Honour it and let
            // the game restyle and resize its own window again.
            DeactivateBorderless("game requested windowed mode");
            return false;
        }

        if (!ActivateBorderless(deviceWindow, requested.BackBufferWidth, requested.BackBufferHeight))
        {
            ShimLog("fullscreen: could not resolve monitor geometry; leaving exclusive request stock");
            return false;
        }

        converted = requested;
        converted.Windowed = TRUE;
        converted.FullScreen_RefreshRateInHz = 0;

        // A windowed swap chain may only take Present rectangles when it was
        // created with D3DSWAPEFFECT_COPY, and COPY allows exactly one back
        // buffer and no multisampling. Only pay that cost when pillarboxing.
        if (g_needsPresentRect)
        {
            converted.SwapEffect = D3DSWAPEFFECT_COPY;
            converted.BackBufferCount = 1;
            converted.MultiSampleType = D3DMULTISAMPLE_NONE;
            converted.MultiSampleQuality = 0;
        }

        return true;
    }

    // Fall back from pillarbox to a plain full-client stretch. Used when the
    // COPY swap chain is rejected, so the player still gets a working screen.
    void DowngradeToStretch(D3DPRESENT_PARAMETERS& params, const D3DPRESENT_PARAMETERS& requested)
    {
        g_destRect = { 0, 0, ClientWidth(), ClientHeight() };
        g_needsPresentRect = false;
        g_needsInputMapping = !(g_logicalW == ClientWidth() && g_logicalH == ClientHeight());

        params = requested;
        params.Windowed = TRUE;
        params.FullScreen_RefreshRateInHz = 0;

        ShimLog("fullscreen: pillarbox swap chain rejected; falling back to full-client stretch");
    }

    HRESULT STDMETHODCALLTYPE HookPresent(
        IDirect3DDevice9* device,
        const RECT* sourceRect,
        const RECT* destRect,
        HWND destWindowOverride,
        const RGNDATA* dirtyRegion)
    {
        if (g_active && g_needsPresentRect && !sourceRect && !destRect)
            destRect = &g_destRect;

        return g_realPresent(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
    }

    HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
    {
        if (!g_realReset)
            return D3DERR_INVALIDCALL;

        if (!params)
            return g_realReset(device, params);

        const D3DPRESENT_PARAMETERS requested = *params;
        HWND deviceWindow = ResolveDeviceWindow(device, params);

        D3DPRESENT_PARAMETERS converted = {};
        if (!ConvertExclusiveToBorderless(deviceWindow, requested, converted))
            return g_realReset(device, params);

        HRESULT hr = g_realReset(device, &converted);

        if (FAILED(hr) && g_needsPresentRect)
        {
            D3DPRESENT_PARAMETERS stretched = {};
            DowngradeToStretch(stretched, requested);
            hr = g_realReset(device, &stretched);
            if (SUCCEEDED(hr))
                PinBorderlessWindow(deviceWindow);
        }

        if (FAILED(hr))
        {
            ShimLog("fullscreen: borderless Reset failed hr=0x%08lX; retrying the stock request", hr);
            DeactivateBorderless("borderless Reset failed");
            return g_realReset(device, params);
        }

        // The game reapplies its own window style and size right after this
        // returns, so re-pin once the device is back.
        PinBorderlessWindow(deviceWindow);
        return hr;
    }

    void HookDeviceMethods(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        void** vtable = *reinterpret_cast<void***>(device);

        void* originalReset = reinterpret_cast<void*>(g_realReset);
        if (PatchPointer(&vtable[kIDirect3DDevice9ResetIndex],
                         reinterpret_cast<void*>(&HookReset), &originalReset))
        {
            if (!g_realReset)
            {
                g_realReset = reinterpret_cast<ResetFn>(originalReset);
                ShimLog("fullscreen: IDirect3DDevice9::Reset hook installed");
            }
        }

        void* originalPresent = reinterpret_cast<void*>(g_realPresent);
        if (PatchPointer(&vtable[kIDirect3DDevice9PresentIndex],
                         reinterpret_cast<void*>(&HookPresent), &originalPresent))
        {
            if (!g_realPresent)
            {
                g_realPresent = reinterpret_cast<PresentFn>(originalPresent);
                ShimLog("fullscreen: IDirect3DDevice9::Present hook installed");
            }
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

        if (!params)
        {
            return g_realCreateDevice(
                d3d, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice);
        }

        const D3DPRESENT_PARAMETERS requested = *params;
        HWND deviceWindow = params->hDeviceWindow ? params->hDeviceWindow : focusWindow;

        D3DPRESENT_PARAMETERS converted = {};
        const bool wasExclusive = ConvertExclusiveToBorderless(deviceWindow, requested, converted);

        HRESULT hr = g_realCreateDevice(
            d3d, adapter, deviceType, focusWindow, behaviorFlags,
            wasExclusive ? &converted : params, outDevice);

        if (FAILED(hr) && wasExclusive && g_needsPresentRect)
        {
            D3DPRESENT_PARAMETERS stretched = {};
            DowngradeToStretch(stretched, requested);
            hr = g_realCreateDevice(
                d3d, adapter, deviceType, focusWindow, behaviorFlags, &stretched, outDevice);
        }

        if (FAILED(hr) && wasExclusive)
        {
            ShimLog("fullscreen: borderless CreateDevice failed hr=0x%08lX; retrying the stock request", hr);
            DeactivateBorderless("borderless CreateDevice failed");
            hr = g_realCreateDevice(
                d3d, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice);
        }

        if (SUCCEEDED(hr) && outDevice && *outDevice)
        {
            HookDeviceMethods(*outDevice);
            if (g_active)
                PinBorderlessWindow(deviceWindow);
        }

        return hr;
    }

    void HookCreateDeviceMethod(IDirect3D9* d3d)
    {
        if (!d3d)
            return;

        void** vtable = *reinterpret_cast<void***>(d3d);
        void* original = reinterpret_cast<void*>(g_realCreateDevice);
        if (PatchPointer(&vtable[kIDirect3D9CreateDeviceIndex],
                         reinterpret_cast<void*>(&HookCreateDevice), &original))
        {
            if (!g_realCreateDevice)
            {
                g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(original);
                ShimLog("fullscreen: IDirect3D9::CreateDevice hook installed");
            }
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

    // ---- window pinning -----------------------------------------------------

    bool OwnsWindow(HWND hwnd)
    {
        return g_active && hwnd && hwnd == g_gameWindow;
    }

    BOOL WINAPI HookSetWindowPos(
        HWND hwnd, HWND insertAfter, int x, int y, int cx, int cy, UINT flags)
    {
        if (OwnsWindow(hwnd))
        {
            x = g_windowRect.left;
            y = g_windowRect.top;
            cx = ClientWidth();
            cy = ClientHeight();
            flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }

        return g_realSetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);
    }

    LONG WINAPI HookSetWindowLongA(HWND hwnd, int index, LONG newLong)
    {
        if (OwnsWindow(hwnd))
        {
            if (index == GWL_STYLE)
            {
                newLong &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
                newLong |= WS_POPUP | WS_VISIBLE;
            }
            else if (index == GWL_EXSTYLE)
            {
                newLong &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                             WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
            }
        }

        return g_realSetWindowLongA(hwnd, index, newLong);
    }

    BOOL WINAPI HookMoveWindow(HWND hwnd, int x, int y, int width, int height, BOOL repaint)
    {
        if (OwnsWindow(hwnd))
        {
            x = g_windowRect.left;
            y = g_windowRect.top;
            width = ClientWidth();
            height = ClientHeight();
        }

        return g_realMoveWindow(hwnd, x, y, width, height, repaint);
    }

    // ---- coordinate space ---------------------------------------------------

    LONG ClampTo(LONG value, LONG limit)
    {
        if (value < 0)
            return 0;
        if (value > limit - 1)
            return limit - 1;
        return value;
    }

    void ActualToLogical(LONG& x, LONG& y)
    {
        const LONG destW = g_destRect.right - g_destRect.left;
        const LONG destH = g_destRect.bottom - g_destRect.top;
        if (destW <= 0 || destH <= 0)
            return;

        x = ClampTo(static_cast<LONG>((static_cast<long long>(x - g_destRect.left) * g_logicalW) / destW),
                    g_logicalW);
        y = ClampTo(static_cast<LONG>((static_cast<long long>(y - g_destRect.top) * g_logicalH) / destH),
                    g_logicalH);
    }

    void LogicalToActual(LONG& x, LONG& y)
    {
        const LONG destW = g_destRect.right - g_destRect.left;
        const LONG destH = g_destRect.bottom - g_destRect.top;
        if (destW <= 0 || destH <= 0 || g_logicalW <= 0 || g_logicalH <= 0)
            return;

        x = g_destRect.left + static_cast<LONG>((static_cast<long long>(x) * destW) / g_logicalW);
        y = g_destRect.top + static_cast<LONG>((static_cast<long long>(y) * destH) / g_logicalH);
    }

    // The game sizes its viewport, its mouse clip rect and its shell hit testing
    // from this, so it has to keep seeing the logical mode size.
    BOOL WINAPI HookGetClientRect(HWND hwnd, LPRECT rect)
    {
        if (OwnsWindow(hwnd) && g_needsInputMapping && rect)
        {
            rect->left = 0;
            rect->top = 0;
            rect->right = g_logicalW;
            rect->bottom = g_logicalH;
            return TRUE;
        }

        return g_realGetClientRect(hwnd, rect);
    }

    // GetWindowScreenCoordinates and LockMouse feed logical coordinates through
    // here to build the ClipCursor rect and to recentre the cursor.
    BOOL WINAPI HookClientToScreen(HWND hwnd, LPPOINT point)
    {
        if (OwnsWindow(hwnd) && g_needsInputMapping && point)
            LogicalToActual(point->x, point->y);

        return g_realClientToScreen(hwnd, point);
    }

    bool IsPositionalMouseMessage(UINT message)
    {
        // WM_MOUSEMOVE .. WM_MBUTTONDBLCLK carry client coordinates in lParam.
        // WM_MOUSEWHEEL/WM_XBUTTON*/WM_MOUSEHWHEEL carry screen coordinates and
        // the game only reads their wParam, so they are left alone.
        return message >= WM_MOUSEMOVE && message <= WM_MBUTTONDBLCLK;
    }

    void RemapMouseMessage(MSG* msg)
    {
        if (!msg || !g_active || !g_needsInputMapping)
            return;
        if (msg->hwnd != g_gameWindow || !IsPositionalMouseMessage(msg->message))
            return;

        LONG x = GET_X_LPARAM(msg->lParam);
        LONG y = GET_Y_LPARAM(msg->lParam);
        ActualToLogical(x, y);
        msg->lParam = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
    }

    BOOL WINAPI HookGetMessageA(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax)
    {
        const BOOL result = g_realGetMessageA(msg, hwnd, filterMin, filterMax);
        if (result > 0)
            RemapMouseMessage(msg);
        return result;
    }

    BOOL WINAPI HookPeekMessageA(
        LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax, UINT removeFlags)
    {
        const BOOL result = g_realPeekMessageA(msg, hwnd, filterMin, filterMax, removeFlags);
        if (result)
            RemapMouseMessage(msg);
        return result;
    }

    struct ImportHook
    {
        const char* dll;
        const char* name;
        void* replacement;
        void** original;
        bool required;
    };

    bool InstallWindowAndInputHooks(HMODULE exe)
    {
        const ImportHook hooks[] = {
            { "user32.dll", "SetWindowPos",   &HookSetWindowPos,   reinterpret_cast<void**>(&g_realSetWindowPos),   true },
            { "user32.dll", "SetWindowLongA", &HookSetWindowLongA, reinterpret_cast<void**>(&g_realSetWindowLongA), true },
            { "user32.dll", "MoveWindow",     &HookMoveWindow,     reinterpret_cast<void**>(&g_realMoveWindow),     false },
            { "user32.dll", "GetClientRect",  &HookGetClientRect,  reinterpret_cast<void**>(&g_realGetClientRect),  true },
            { "user32.dll", "ClientToScreen", &HookClientToScreen, reinterpret_cast<void**>(&g_realClientToScreen), true },
            { "user32.dll", "GetMessageA",    &HookGetMessageA,    reinterpret_cast<void**>(&g_realGetMessageA),    true },
            { "user32.dll", "PeekMessageA",   &HookPeekMessageA,   reinterpret_cast<void**>(&g_realPeekMessageA),   true },
        };

        bool allRequired = true;
        for (const ImportHook& hook : hooks)
        {
            void* original = nullptr;
            if (HookImport(exe, hook.dll, hook.name, hook.replacement, &original) && original)
            {
                *hook.original = original;
                continue;
            }

            ShimLog("fullscreen: %s import hook unavailable", hook.name);
            if (hook.required)
                allRequired = false;
        }

        return allRequired;
    }
}

bool InstallFullscreenMenuFix()
{
    LoadSettings();
    if (!g_enabled)
    {
        ShimLog("fullscreen: disabled by bz15_shim.ini; leaving the game stock");
        return false;
    }

    HMODULE exe = GetModuleHandleA(nullptr);

    if (!InstallWindowAndInputHooks(exe))
    {
        ShimLog("fullscreen: required window/input hooks missing; refusing to convert fullscreen");
        return false;
    }

    void* original = nullptr;
    if (!HookImport(exe, "d3d9.dll", "Direct3DCreate9",
                    reinterpret_cast<void*>(&HookDirect3DCreate9), &original))
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
    // Nothing to undo: this path never changes the desktop display mode, and the
    // process is going away with its window.
    DeactivateBorderless("shutdown");
}
