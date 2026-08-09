#include "fullscreen_fix.h"
#include "shim_log.h"
#include "winmm_proxy.h"

#include <Windows.h>
#include <vector>
#include <cstring>

namespace
{
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
        }
        else
        {
            ShimLog("startup: unsupported executable/version; no game patch installed");
        }
        break;

    case DLL_PROCESS_DETACH:
        FreeRealWinmm();
        break;
    }

    return TRUE;
}
