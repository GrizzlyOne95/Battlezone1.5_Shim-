// fullscreen_fix.cpp
//
// Battlezone 1.5.2.27's shell -- main menu, options, mission select -- is a real
// Win32 dialog. Read_Shell_Bitmap loads bitmap\*.bmp into a BITMAPINFO, the
// ShellButton/TextLabel/OptionBox classes owner-draw themselves through an HDC
// (ShellButton::DrawLabelText takes one), and the whole thing is put on screen by
// CreateDialogParamA as a child of the game window, driven by ShellDlgProc.
//
// That is why D3D_Change_Mode_Ex calls IDirect3DDevice9::SetDialogBoxMode(TRUE)
// whenever ResolutionMode == 0. The API exists for exactly this: "allows the use
// of GDI dialog boxes in full-screen mode applications", with the requirement
// that they be "child to the device window".
//
// On modern WDDM drivers that mechanism no longer works, so under an exclusive
// swap chain the dialog is simply never composited and the player sees nothing
// until they Alt+Enter into a window. Missions are unaffected because they are
// pure D3D with no GDI and go through Present normally -- during the shell the
// device presents twice and then sits idle.
//
// Consequences for any fix:
//
//   * The shell cannot be scaled at the D3D layer. Its pixels never pass through
//     the back buffer, so Present rectangles, stretched blits and back-buffer
//     upscales all miss it entirely.
//
//   * It cannot be scaled at the Win32 layer either. The layout is fixed-pixel
//     and the art is 640x480 bitmaps, so resizing the dialog would not reflow it.
//
// The only thing that makes a 640x480 dialog fill a 4K screen is the display
// itself, which is what exclusive fullscreen was implicitly doing all along. So:
// take the device out of exclusive mode (making the dialog visible again), and
// put the monitor into the mode the game asked for (making it fill the screen).
// The panel's own scaler does the upscale, exactly as before.
//
// Because the window always ends up the same size as the mode the game asked
// for, the game's own coordinate maths stays correct and no input remapping is
// needed anywhere.
//
// Settings live in bz15_shim.ini next to bzone.exe:
//
//   [Fullscreen]
//   Mode=displaymode  ; displaymode (default) | center | off

#include "fullscreen_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace
{
    using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    using SetWindowLongAFn = LONG(WINAPI*)(HWND, int, LONG);
    using MoveWindowFn = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);

    constexpr size_t kIDirect3D9CreateDeviceIndex = 16;
    constexpr size_t kIDirect3DDevice9ResetIndex = 16;

    Direct3DCreate9Fn g_realDirect3DCreate9 = nullptr;
    CreateDeviceFn g_realCreateDevice = nullptr;
    ResetFn g_realReset = nullptr;

    SetWindowPosFn g_realSetWindowPos = nullptr;
    SetWindowLongAFn g_realSetWindowLongA = nullptr;
    MoveWindowFn g_realMoveWindow = nullptr;

    enum class Mode
    {
        // Keep the desktop resolution and mirror the small game window into a
        // fullscreen host with a DWM thumbnail, forwarding mouse input back.
        Mirror,
        // Drive the monitor into the mode the game asked for, so the panel
        // upscales the shell dialog the way exclusive fullscreen used to.
        DisplayMode,
        // Leave the desktop alone and just centre a borderless window of the
        // requested size. The shell stays physically 640x480.
        Center,
        // Leave the game completely stock.
        Off,
    };

    Mode g_mode = Mode::DisplayMode;

    bool g_active = false;
    HWND g_gameWindow = nullptr;
    RECT g_windowRect = {};

    // Size the game is actually rendering at, i.e. the mirror source size.
    LONG g_logicalW = 0;
    LONG g_logicalH = 0;

    struct DisplayModeState
    {
        bool captured = false;
        bool changed = false;
        char deviceName[CCHDEVICENAME] = {};
        DEVMODEA original = {};
    };

    DisplayModeState g_display;

    LONG WindowWidth() { return g_windowRect.right - g_windowRect.left; }
    LONG WindowHeight() { return g_windowRect.bottom - g_windowRect.top; }

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
        GetPrivateProfileStringA("Fullscreen", "Mode", "displaymode", mode, sizeof(mode), ini);

        if (_stricmp(mode, "off") == 0 || _stricmp(mode, "exclusive") == 0)
            g_mode = Mode::Off;
        else if (_stricmp(mode, "center") == 0 || _stricmp(mode, "centre") == 0)
            g_mode = Mode::Center;
        else if (_stricmp(mode, "mirror") == 0)
            g_mode = Mode::Mirror;
        else
            g_mode = Mode::DisplayMode;

        ShimLog("fullscreen: settings mode=%s (%s)", mode, ini);
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

    // ---- display mode -------------------------------------------------------

    bool GetMonitorDevice(HWND hwnd, MONITORINFOEXA& info)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        info = {};
        info.cbSize = sizeof(info);
        return GetMonitorInfoA(monitor, reinterpret_cast<LPMONITORINFO>(&info)) != FALSE;
    }

    void RestoreDesktopMode()
    {
        if (!g_display.captured)
            return;

        if (g_display.changed)
        {
            const LONG result = ChangeDisplaySettingsExA(
                g_display.deviceName, &g_display.original, nullptr, 0, nullptr);

            ShimLog(
                "display: restoring %s to %lux%lu@%lu result=%ld",
                g_display.deviceName,
                g_display.original.dmPelsWidth,
                g_display.original.dmPelsHeight,
                g_display.original.dmDisplayFrequency,
                result);
        }

        g_display = {};
    }

    bool CaptureDesktopMode(HWND hwnd)
    {
        MONITORINFOEXA monitor = {};
        if (!GetMonitorDevice(hwnd, monitor))
            return false;

        if (g_display.captured)
        {
            if (_stricmp(g_display.deviceName, monitor.szDevice) == 0)
                return true;
            RestoreDesktopMode();
        }

        DEVMODEA mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
            return false;

        g_display.captured = true;
        g_display.changed = false;
        strcpy_s(g_display.deviceName, monitor.szDevice);
        g_display.original = mode;

        ShimLog(
            "display: captured %s desktop mode %lux%lu@%lu",
            g_display.deviceName,
            mode.dmPelsWidth,
            mode.dmPelsHeight,
            mode.dmDisplayFrequency);
        return true;
    }

    // Put the monitor into `width x height`, or back to the desktop mode when
    // that is already what was asked for. Returns true if the monitor now shows
    // the requested geometry.
    bool ApplyLegacyDisplayMode(HWND hwnd, UINT width, UINT height, UINT refresh)
    {
        if (!hwnd || !width || !height)
            return false;

        if (!CaptureDesktopMode(hwnd))
        {
            ShimLog("display: failed to capture the current monitor mode");
            return false;
        }

        // A mission running at the desktop resolution needs no mode change, and
        // if we changed it for the shell we have to undo that first.
        if (g_display.original.dmPelsWidth == width && g_display.original.dmPelsHeight == height)
        {
            if (g_display.changed)
            {
                const LONG result = ChangeDisplaySettingsExA(
                    g_display.deviceName, &g_display.original, nullptr, 0, nullptr);
                g_display.changed = false;
                ShimLog(
                    "display: %ux%u is the desktop mode; restored %s result=%ld",
                    width, height, g_display.deviceName, result);
            }
            return true;
        }

        DEVMODEA current = {};
        current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsExA(g_display.deviceName, ENUM_CURRENT_SETTINGS, &current, 0))
            return false;

        if (current.dmPelsWidth == width && current.dmPelsHeight == height)
        {
            g_display.changed = true;
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
            g_display.deviceName,
            current.dmPelsWidth,
            current.dmPelsHeight,
            current.dmDisplayFrequency,
            width,
            height,
            refresh);

        LONG result = ChangeDisplaySettingsExA(
            g_display.deviceName, &desired, nullptr, CDS_FULLSCREEN, nullptr);

        if (result != DISP_CHANGE_SUCCESSFUL && refresh)
        {
            desired.dmFields &= ~DM_DISPLAYFREQUENCY;
            result = ChangeDisplaySettingsExA(
                g_display.deviceName, &desired, nullptr, CDS_FULLSCREEN, nullptr);
            ShimLog("display: retried without an explicit refresh rate result=%ld", result);
        }

        if (result != DISP_CHANGE_SUCCESSFUL)
        {
            ShimLog("display: mode switch failed result=%ld", result);
            return false;
        }

        g_display.changed = true;
        return true;
    }

    // ---- mirror host --------------------------------------------------------
    //
    // The shell is a Win32 dialog, so it cannot be scaled by D3D and it cannot
    // be re-laid-out. What it can be is *mirrored*: DwmRegisterThumbnail is the
    // API behind taskbar previews, and DWM will live-composite a top-level
    // window into an arbitrary destination rectangle, scaled on the GPU, even
    // while the source is occluded. That covers the owner-drawn dialog and the
    // MCI video playing behind the menu without knowing anything about either.
    //
    // The host is WS_EX_NOACTIVATE, so focus stays on the game window and the
    // keyboard keeps working untouched. Only the mouse has to be mapped back
    // out of screen space into the game's 640x480 space and reposted.

    constexpr char kHostClassName[] = "BZ15ShimMirrorHost";

    HWND g_host = nullptr;
    HTHUMBNAIL g_thumbnail = nullptr;
    ATOM g_hostClass = 0;
    RECT g_mirrorDest = {};
    RECT g_monitorRect = {};

    // What DWM says it is actually mirroring. Normally the same as the game's
    // client size, but it is DWM's number that decides the on-screen geometry.
    LONG g_mirrorSourceW = 0;
    LONG g_mirrorSourceH = 0;

    RECT AspectFit(LONG srcW, LONG srcH, LONG dstW, LONG dstH)
    {
        RECT out = { 0, 0, dstW, dstH };
        if (srcW <= 0 || srcH <= 0)
            return out;

        const long long byWidth = static_cast<long long>(dstW) * srcH;
        const long long byHeight = static_cast<long long>(dstH) * srcW;

        LONG width = dstW;
        LONG height = dstH;
        if (byWidth > byHeight)
        {
            height = dstH;
            width = static_cast<LONG>(byHeight / srcH);
        }
        else
        {
            width = dstW;
            height = static_cast<LONG>(byWidth / srcW);
        }

        out.left = (dstW - width) / 2;
        out.top = (dstH - height) / 2;
        out.right = out.left + width;
        out.bottom = out.top + height;
        return out;
    }

    // Map a point in host client space back into the game's client space, then
    // walk down to whichever child window actually sits under it so the dialog's
    // own controls receive coordinates in their own space.
    void ForwardMouseToGame(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!g_gameWindow || !IsWindow(g_gameWindow))
            return;

        const LONG destW = g_mirrorDest.right - g_mirrorDest.left;
        const LONG destH = g_mirrorDest.bottom - g_mirrorDest.top;
        if (destW <= 0 || destH <= 0 || g_logicalW <= 0 || g_logicalH <= 0)
            return;

        POINT hostPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        {
            // Wheel messages carry screen coordinates.
            ScreenToClient(g_host, &hostPoint);
        }

        LONG x = static_cast<LONG>(
            (static_cast<long long>(hostPoint.x - g_mirrorDest.left) * g_logicalW) / destW);
        LONG y = static_cast<LONG>(
            (static_cast<long long>(hostPoint.y - g_mirrorDest.top) * g_logicalH) / destH);

        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > g_logicalW - 1) x = g_logicalW - 1;
        if (y > g_logicalH - 1) y = g_logicalH - 1;

        HWND target = g_gameWindow;
        POINT local = { x, y };
        for (int depth = 0; depth < 8; ++depth)
        {
            HWND child = ChildWindowFromPointEx(
                target, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
            if (!child || child == target)
                break;

            POINT screen = local;
            ClientToScreen(target, &screen);
            ScreenToClient(child, &screen);
            local = screen;
            target = child;
        }

        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        {
            POINT screen = local;
            ClientToScreen(target, &screen);
            PostMessageA(target, message, wParam,
                         MAKELPARAM(static_cast<WORD>(screen.x), static_cast<WORD>(screen.y)));
            return;
        }

        PostMessageA(target, message, wParam,
                     MAKELPARAM(static_cast<WORD>(local.x), static_cast<WORD>(local.y)));
    }

    LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            // Never steal focus: the dialog needs to keep the keyboard.
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
            return HTCLIENT;

        case WM_SETCURSOR:
            SetCursor(LoadCursorA(nullptr, IDC_ARROW));
            return TRUE;

        case WM_ERASEBKGND:
        {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            ForwardMouseToGame(message, wParam, lParam);
            return 0;

        default:
            break;
        }

        return DefWindowProcA(hwnd, message, wParam, lParam);
    }

    void HideMirror()
    {
        if (g_thumbnail)
        {
            DwmUnregisterThumbnail(g_thumbnail);
            g_thumbnail = nullptr;
        }

        if (g_host && IsWindow(g_host))
            ShowWindow(g_host, SW_HIDE);
    }

    void DestroyMirror()
    {
        HideMirror();

        if (g_host && IsWindow(g_host))
            DestroyWindow(g_host);
        g_host = nullptr;
    }

    bool EnsureHostWindow()
    {
        if (g_host && IsWindow(g_host))
            return true;

        HINSTANCE instance = GetModuleHandleA(nullptr);

        if (!g_hostClass)
        {
            WNDCLASSEXA wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &HostWndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = kHostClassName;

            g_hostClass = RegisterClassExA(&wc);
            if (!g_hostClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                ShimLog("mirror: RegisterClassEx failed (err=%lu)", GetLastError());
                return false;
            }
        }

        g_host = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kHostClassName,
            "Battlezone",
            WS_POPUP,
            g_monitorRect.left,
            g_monitorRect.top,
            g_monitorRect.right - g_monitorRect.left,
            g_monitorRect.bottom - g_monitorRect.top,
            g_gameWindow,
            nullptr,
            instance,
            nullptr);

        if (!g_host)
        {
            ShimLog("mirror: CreateWindowEx failed (err=%lu)", GetLastError());
            return false;
        }

        ShimLog("mirror: host window created");
        return true;
    }

    bool ShowMirror()
    {
        if (!EnsureHostWindow())
            return false;

        const LONG monitorW = g_monitorRect.right - g_monitorRect.left;
        const LONG monitorH = g_monitorRect.bottom - g_monitorRect.top;

        SetWindowPos(
            g_host,
            HWND_TOPMOST,
            g_monitorRect.left,
            g_monitorRect.top,
            monitorW,
            monitorH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        if (g_thumbnail)
        {
            DwmUnregisterThumbnail(g_thumbnail);
            g_thumbnail = nullptr;
        }

        HRESULT hr = DwmRegisterThumbnail(g_host, g_gameWindow, &g_thumbnail);
        if (FAILED(hr) || !g_thumbnail)
        {
            ShimLog("mirror: DwmRegisterThumbnail failed hr=0x%08lX", hr);
            ShowWindow(g_host, SW_HIDE);
            return false;
        }

        // DWM scales the thumbnail to fit inside rcDestination while preserving
        // *its own* notion of the source aspect ratio, anchored top-left. If that
        // notion disagrees with the game's client size the image comes out
        // letterboxed inside our rectangle and every input coordinate derived
        // from that rectangle is wrong, so take the size from DWM rather than
        // assuming it matches the back buffer.
        SIZE sourceSize = { g_logicalW, g_logicalH };
        const HRESULT sizeHr = DwmQueryThumbnailSourceSize(g_thumbnail, &sourceSize);
        if (FAILED(sizeHr) || sourceSize.cx <= 0 || sourceSize.cy <= 0)
        {
            ShimLog("mirror: DwmQueryThumbnailSourceSize failed hr=0x%08lX; assuming %ldx%ld",
                    sizeHr, g_logicalW, g_logicalH);
            sourceSize.cx = g_logicalW;
            sourceSize.cy = g_logicalH;
        }
        else if (sourceSize.cx != g_logicalW || sourceSize.cy != g_logicalH)
        {
            ShimLog(
                "mirror: DWM reports a %ldx%ld source, not the %ldx%ld client; fitting to DWM's size",
                sourceSize.cx, sourceSize.cy, g_logicalW, g_logicalH);
        }

        g_mirrorSourceW = sourceSize.cx;
        g_mirrorSourceH = sourceSize.cy;
        g_mirrorDest = AspectFit(sourceSize.cx, sourceSize.cy, monitorW, monitorH);

        DWM_THUMBNAIL_PROPERTIES properties = {};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                             DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
        properties.rcDestination = g_mirrorDest;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = TRUE;
        properties.opacity = 255;

        hr = DwmUpdateThumbnailProperties(g_thumbnail, &properties);
        if (FAILED(hr))
        {
            ShimLog("mirror: DwmUpdateThumbnailProperties failed hr=0x%08lX", hr);
            HideMirror();
            return false;
        }

        InvalidateRect(g_host, nullptr, TRUE);

        ShimLog(
            "mirror: mirroring %ldx%ld (DWM source %ldx%ld) into %ld,%ld %ldx%ld on a %ldx%ld screen",
            g_logicalW,
            g_logicalH,
            g_mirrorSourceW,
            g_mirrorSourceH,
            g_mirrorDest.left,
            g_mirrorDest.top,
            g_mirrorDest.right - g_mirrorDest.left,
            g_mirrorDest.bottom - g_mirrorDest.top,
            monitorW,
            monitorH);

        return true;
    }

    // ---- window -------------------------------------------------------------

    void PinWindow(HWND hwnd)
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

        SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND,
                         reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));

        g_realSetWindowPos(
            hwnd,
            HWND_TOP,
            g_windowRect.left,
            g_windowRect.top,
            WindowWidth(),
            WindowHeight(),
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }

    // Work out where a window of `width x height` should sit, after any display
    // mode change has already been applied.
    bool ComputeWindowRect(HWND hwnd, UINT width, UINT height, RECT& out)
    {
        MONITORINFOEXA monitor = {};
        if (!GetMonitorDevice(hwnd, monitor))
            return false;

        const LONG monitorW = monitor.rcMonitor.right - monitor.rcMonitor.left;
        const LONG monitorH = monitor.rcMonitor.bottom - monitor.rcMonitor.top;

        // In displaymode the monitor should now match the requested size, so this
        // lands at the origin and fills the screen. If the mode switch was
        // refused it degrades to a centred window rather than a broken one.
        out.left = monitor.rcMonitor.left + (monitorW - static_cast<LONG>(width)) / 2;
        out.top = monitor.rcMonitor.top + (monitorH - static_cast<LONG>(height)) / 2;
        if (out.left < monitor.rcMonitor.left)
            out.left = monitor.rcMonitor.left;
        if (out.top < monitor.rcMonitor.top)
            out.top = monitor.rcMonitor.top;
        out.right = out.left + static_cast<LONG>(width);
        out.bottom = out.top + static_cast<LONG>(height);
        return true;
    }

    void Deactivate(const char* reason)
    {
        if (!g_active)
            return;

        g_active = false;
        HideMirror();
        ShimLog("fullscreen: conversion disengaged (%s)", reason);
    }

    // Rewrite an exclusive request into a windowed one on a borderless window of
    // the same size. Returns true when `converted` should be used instead.
    bool ConvertExclusiveRequest(
        HWND deviceWindow,
        const D3DPRESENT_PARAMETERS& requested,
        D3DPRESENT_PARAMETERS& converted)
    {
        if (g_mode == Mode::Off)
            return false;

        if (requested.Windowed)
        {
            // Alt+Enter: the player asked for a real window. Give the desktop
            // back and let the game restyle and resize itself.
            Deactivate("game requested windowed mode");
            RestoreDesktopMode();
            return false;
        }

        if (!deviceWindow || !IsWindow(deviceWindow))
        {
            ShimLog("fullscreen: no usable device window; leaving the request stock");
            return false;
        }

        g_gameWindow = deviceWindow;
        g_logicalW = static_cast<LONG>(requested.BackBufferWidth);
        g_logicalH = static_cast<LONG>(requested.BackBufferHeight);

        if (g_mode == Mode::DisplayMode)
        {
            ApplyLegacyDisplayMode(
                deviceWindow,
                requested.BackBufferWidth,
                requested.BackBufferHeight,
                requested.FullScreen_RefreshRateInHz);
        }

        MONITORINFOEXA monitor = {};
        if (!GetMonitorDevice(deviceWindow, monitor))
        {
            ShimLog("fullscreen: could not resolve monitor geometry; leaving the request stock");
            return false;
        }
        g_monitorRect = monitor.rcMonitor;

        bool wantMirror = false;

        if (g_mode == Mode::Mirror)
        {
            const LONG monitorW = g_monitorRect.right - g_monitorRect.left;
            const LONG monitorH = g_monitorRect.bottom - g_monitorRect.top;

            if (g_logicalW >= monitorW && g_logicalH >= monitorH)
            {
                // A mission at the native resolution has nothing to magnify, so
                // the game window just becomes a normal borderless fullscreen.
                HideMirror();
                g_windowRect = g_monitorRect;
            }
            else
            {
                // Park the source window at the monitor origin. It ends up fully
                // covered by the host, which is fine: DWM keeps compositing
                // occluded windows, which is what makes taskbar previews work.
                g_windowRect.left = g_monitorRect.left;
                g_windowRect.top = g_monitorRect.top;
                g_windowRect.right = g_windowRect.left + g_logicalW;
                g_windowRect.bottom = g_windowRect.top + g_logicalH;
                wantMirror = true;
            }
        }
        else if (!ComputeWindowRect(
                     deviceWindow, requested.BackBufferWidth, requested.BackBufferHeight, g_windowRect))
        {
            ShimLog("fullscreen: could not resolve monitor geometry; leaving the request stock");
            return false;
        }

        g_active = true;
        PinWindow(deviceWindow);

        if (wantMirror && !ShowMirror())
        {
            // Mirroring is the whole point of this mode, so if DWM refuses,
            // centre the window rather than leaving it stranded in a corner.
            ShimLog("mirror: unavailable; falling back to a centred window");
            if (ComputeWindowRect(
                    deviceWindow, requested.BackBufferWidth, requested.BackBufferHeight, g_windowRect))
            {
                PinWindow(deviceWindow);
            }
        }

        converted = requested;
        converted.Windowed = TRUE;
        converted.FullScreen_RefreshRateInHz = 0;

        ShimLog(
            "fullscreen: exclusive %ux%u -> borderless window at %ld,%ld %ldx%ld",
            requested.BackBufferWidth,
            requested.BackBufferHeight,
            g_windowRect.left,
            g_windowRect.top,
            WindowWidth(),
            WindowHeight());

        return true;
    }

    void LogGranted(IDirect3DDevice9* device, const char* stage)
    {
        if (!device || !g_gameWindow)
            return;

        RECT client = {};
        GetClientRect(g_gameWindow, &client);
        RECT window = {};
        GetWindowRect(g_gameWindow, &window);

        ShimLog(
            "fullscreen: [%s] client %ldx%ld, window %ld,%ld %ldx%ld, style=0x%08lX",
            stage,
            client.right - client.left,
            client.bottom - client.top,
            window.left,
            window.top,
            window.right - window.left,
            window.bottom - window.top,
            GetWindowLongA(g_gameWindow, GWL_STYLE));
    }

    HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
    {
        if (!g_realReset)
            return D3DERR_INVALIDCALL;

        if (!params)
            return g_realReset(device, params);

        const D3DPRESENT_PARAMETERS requested = *params;
        HWND deviceWindow = params->hDeviceWindow ? params->hDeviceWindow : g_gameWindow;

        D3DPRESENT_PARAMETERS converted = {};
        if (!ConvertExclusiveRequest(deviceWindow, requested, converted))
            return g_realReset(device, params);

        HRESULT hr = g_realReset(device, &converted);

        if (FAILED(hr))
        {
            // A mode switch is still settling often enough to be worth one retry
            // before giving up and handing the game its original request back.
            Sleep(120);
            hr = g_realReset(device, &converted);
            if (FAILED(hr))
            {
                ShimLog("fullscreen: windowed Reset failed hr=0x%08lX; retrying the stock request", hr);
                Deactivate("windowed Reset failed");
                RestoreDesktopMode();
                return g_realReset(device, params);
            }
            ShimLog("fullscreen: windowed Reset succeeded on retry");
        }

        // D3D_Change_Mode_Ex reapplies its own window style and size right after
        // this returns, so pin the layout again once the device is back.
        PinWindow(deviceWindow);
        LogGranted(device, "reset");
        return hr;
    }

    void HookDeviceMethods(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        void** vtable = *reinterpret_cast<void***>(device);
        void* original = reinterpret_cast<void*>(g_realReset);
        if (PatchPointer(&vtable[kIDirect3DDevice9ResetIndex],
                         reinterpret_cast<void*>(&HookReset), &original))
        {
            if (!g_realReset)
            {
                g_realReset = reinterpret_cast<ResetFn>(original);
                ShimLog("fullscreen: IDirect3DDevice9::Reset hook installed");
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
        const bool converting = ConvertExclusiveRequest(deviceWindow, requested, converted);

        HRESULT hr = g_realCreateDevice(
            d3d, adapter, deviceType, focusWindow, behaviorFlags,
            converting ? &converted : params, outDevice);

        if (FAILED(hr) && converting)
        {
            ShimLog("fullscreen: windowed CreateDevice failed hr=0x%08lX; retrying the stock request", hr);
            Deactivate("windowed CreateDevice failed");
            RestoreDesktopMode();
            hr = g_realCreateDevice(
                d3d, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice);
        }

        if (SUCCEEDED(hr) && outDevice && *outDevice)
        {
            HookDeviceMethods(*outDevice);
            if (g_active)
            {
                PinWindow(deviceWindow);
                LogGranted(*outDevice, "create");
            }
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

    // ---- keeping the game from restyling its own window ---------------------

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
            cx = WindowWidth();
            cy = WindowHeight();
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
            width = WindowWidth();
            height = WindowHeight();
        }

        return g_realMoveWindow(hwnd, x, y, width, height, repaint);
    }

    struct ImportHook
    {
        const char* name;
        void* replacement;
        void** original;
        bool required;
    };

    bool InstallWindowHooks(HMODULE exe)
    {
        const ImportHook hooks[] = {
            { "SetWindowPos",   &HookSetWindowPos,   reinterpret_cast<void**>(&g_realSetWindowPos),   true },
            { "SetWindowLongA", &HookSetWindowLongA, reinterpret_cast<void**>(&g_realSetWindowLongA), true },
            { "MoveWindow",     &HookMoveWindow,     reinterpret_cast<void**>(&g_realMoveWindow),     false },
        };

        bool ok = true;
        for (const ImportHook& hook : hooks)
        {
            void* original = nullptr;
            if (HookImport(exe, "user32.dll", hook.name, hook.replacement, &original) && original)
            {
                *hook.original = original;
                continue;
            }

            ShimLog("fullscreen: %s import hook unavailable", hook.name);
            if (hook.required)
                ok = false;
        }

        return ok;
    }
}

bool InstallFullscreenMenuFix()
{
    LoadSettings();
    if (g_mode == Mode::Off)
    {
        ShimLog("fullscreen: disabled by bz15_shim.ini; leaving the game stock");
        return false;
    }

    HMODULE exe = GetModuleHandleA(nullptr);

    if (!InstallWindowHooks(exe))
    {
        ShimLog("fullscreen: required window hooks missing; refusing to convert fullscreen");
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
    Deactivate("shutdown");
    DestroyMirror();
    RestoreDesktopMode();
}
