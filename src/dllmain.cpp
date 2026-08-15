#include "fullscreen_fix.h"
#include "hud_scale.h"
#include "movie_fix.h"
#include "movie_geometry_probe.h"
#include "movie_present_vfw_fix.h"
#include "shell_diag.h"
#include "shim_log.h"
#include "video_codec_shim.h"
#include "winmm_proxy.h"

#include <Windows.h>
#include <vector>
#include <cstring>

namespace
{
    // [Movies] Codec=off restores the old behaviour (convert with ffmpeg.exe),
    // which is worth having if the decoder ever misbehaves on a clip.
    bool MovieCodecEnabled()
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return true;

        char* slash = strrchr(path, '\\');
        if (!slash)
            return true;
        slash[1] = '\0';
        strcat_s(path, "bz15_shim.ini");

        char value[32] = {};
        GetPrivateProfileStringA("Movies", "Codec", "on", value, sizeof(value), path);
        return _stricmp(value, "off") != 0 && _stricmp(value, "0") != 0;
    }

    bool GetExeVersion(WORD& major, WORD& minor, WORD& build, WORD& revision)
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return false;

        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeA(path, &ignored);
        if (!size)
            return false;

        std::vector<unsigned char> buffer(size);
        if (!GetFileVersionInfoA(path, 0, size, buffer.data()))
            return false;

        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoSize = 0;
        if (!VerQueryValueA(buffer.data(), "\\", reinterpret_cast<void**>(&info), &infoSize) ||
            !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
        {
            return false;
        }

        major = HIWORD(info->dwFileVersionMS);
        minor = LOWORD(info->dwFileVersionMS);
        build = HIWORD(info->dwFileVersionLS);
        revision = LOWORD(info->dwFileVersionLS);
        return true;
    }

    bool IsSupportedBattlezone()
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return false;

        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        if (_stricmp(base, "bzone.exe") != 0)
        {
            ShimLog("compat: executable is %s, expected bzone.exe; proxy-only mode", base);
            return false;
        }

        WORD major = 0;
        WORD minor = 0;
        WORD build = 0;
        WORD revision = 0;
        if (!GetExeVersion(major, minor, build, revision))
        {
            ShimLog("compat: could not read bzone.exe file version; proxy-only mode");
            return false;
        }

        ShimLog("compat: bzone.exe version %u.%u.%u.%u", major, minor, build, revision);
        return major == 1 && minor == 5 && build == 2 && revision == 27;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        ShimLog("startup: Battlezone 1.5 shim loading");

        if (!LoadRealWinmm())
        {
            ShimLog("startup: failed to load real winmm.dll; refusing process attach");
            return FALSE;
        }

        if (IsSupportedBattlezone())
        {
            if (InstallFullscreenMenuFix())
                ShimLog("startup: fullscreen menu compatibility fix active");
            else
                ShimLog("startup: fullscreen menu fix could not be installed; game remains stock");

            // Install diagnostics after the compatibility fix. When the fix is
            // active this passively wraps its D3D9 hook chain; with Mode=off it
            // observes the game's original exclusive SetDialogBoxMode path.
            // DiagnoseShell=off (the default) installs nothing.
            if (InstallShellPresentationDiagnostics())
                ShimLog("startup: passive shell presentation diagnostics active");

            // Patches bzone.exe's own 2D pass, so it is independent of every
            // hook below and is skipped entirely when [Hud] Scale=1.
            if (InstallHudScale())
                ShimLog("startup: HUD scaling active");
            else
                ShimLog("startup: HUD scaling not installed; HUD remains stock");

            // Install the decoder before the movie hook. InstallLegacyMovieFix
            // decides whether to substitute a converted copy by asking VfW
            // whether the clip's codec exists, so registering IV50 first is
            // what retires the transcode path: the probe now succeeds and the
            // stock files are played untouched.
            if (MovieCodecEnabled())
            {
                if (InstallVideoCodecShim())
                    ShimLog("startup: Indeo Video 5 decoder active");
                else
                    ShimLog("startup: Indeo Video 5 decoder could not be installed;"
                            " falling back to file conversion");
            }
            else
            {
                ShimLog("startup: Indeo Video 5 decoder disabled by bz15_shim.ini");
            }

            if (InstallLegacyMovieFix())
                ShimLog("startup: legacy movie compatibility hook active");
            else
                ShimLog("startup: legacy movie hook could not be installed");

            if (InstallMovieGeometryProbe())
                ShimLog("startup: passive movie geometry probe active");
            else
                ShimLog("startup: passive movie geometry probe could not be installed");

            // Outermost movie hook. MCI remains authoritative for open/play/
            // stop/seek/timing. If MCI_UPDATE actually produces pixels we keep
            // that path. If it returns a black frame, the presenter reads the
            // current sample through Avifil32 from the original codec-free AVI
            // or the shim's existing IV50 compatibility cache and blits only
            // Battlezone's MCI_PUT destination rectangle.
            if (InstallMovieVfwPresentationFix())
                ShimLog("startup: adaptive VfW movie presentation fallback active");
            else
                ShimLog("startup: adaptive VfW movie presentation fallback could not be installed");
        }
        else
        {
            ShimLog("startup: unsupported executable/version; no game patch installed");
        }
        break;

    case DLL_PROCESS_DETACH:
        ShutdownMovieVfwPresentationFix();
        ShutdownMovieGeometryProbe();
        ShutdownLegacyMovieFix();
        ShutdownVideoCodecShim();
        ShutdownHudScale();
        ShutdownShellPresentationDiagnostics();
        ShutdownFullscreenMenuFix();
        FreeRealWinmm();
        break;
    }

    return TRUE;
}
