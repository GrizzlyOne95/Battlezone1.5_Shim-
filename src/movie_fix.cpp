#include "movie_fix.h"
#include "shim_log.h"
#include "winmm_proxy.h"

#include <Windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using MciSendCommandAFn = MCIERROR (WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);

    MciSendCommandAFn g_originalMciSendCommandA = nullptr;

    bool HookImport(HMODULE module, const char* importedDll, const char* procName,
                    void* replacement, void** original)
    {
        if (!module || !importedDll || !procName || !replacement || !original)
            return false;

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size)
            return false;

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; desc->Name; ++desc)
        {
            const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
            if (_stricmp(dllName, importedDll) != 0)
                continue;

            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            auto* names = desc->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                : iat;

            for (; iat->u1.Function; ++iat, ++names)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;

                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), procName) != 0)
                    continue;

                DWORD oldProtect = 0;
                if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                                    PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    return false;
                }

                *original = reinterpret_cast<void*>(iat->u1.Function);
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

                DWORD ignored = 0;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), oldProtect, &ignored);
                FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function, sizeof(iat->u1.Function));
                return true;
            }
        }

        return false;
    }

    bool FileExistsNonEmpty(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            return false;
        return data.nFileSizeHigh != 0 || data.nFileSizeLow != 0;
    }

    bool ResolveFullPath(const char* path, std::string& fullPath)
    {
        if (!path || !*path)
            return false;

        char buffer[4096] = {};
        const DWORD length = GetFullPathNameA(path, static_cast<DWORD>(sizeof(buffer)), buffer, nullptr);
        if (!length || length >= sizeof(buffer))
            return false;

        fullPath.assign(buffer, length);
        return true;
    }

    bool IsIv50Avi(const std::string& path)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        constexpr DWORD kProbeBytes = 256 * 1024;
        std::vector<unsigned char> data(kProbeBytes);
        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(file, data.data(), kProbeBytes, &bytesRead, nullptr);
        CloseHandle(file);

        if (!readOk || bytesRead < 12)
            return false;

        if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "AVI ", 4) != 0)
            return false;

        for (DWORD i = 0; i + 4 <= bytesRead; ++i)
        {
            if (std::memcmp(data.data() + i, "IV50", 4) == 0)
                return true;
        }

        return false;
    }

    std::string FileNameOnly(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void SanitizeFileName(std::string& name)
    {
        for (char& ch : name)
        {
            if (!(ch >= 'a' && ch <= 'z') &&
                !(ch >= 'A' && ch <= 'Z') &&
                !(ch >= '0' && ch <= '9') && ch != '.' && ch != '_' && ch != '-')
            {
                ch = '_';
            }
        }
    }

    bool EnsureDirectory(const std::string& path)
    {
        if (CreateDirectoryA(path.c_str(), nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool BuildCachePath(const std::string& sourcePath, std::string& cachePath)
    {
        char localAppData[2048] = {};
        DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                               static_cast<DWORD>(sizeof(localAppData)));
        if (!length || length >= sizeof(localAppData))
        {
            length = GetTempPathA(static_cast<DWORD>(sizeof(localAppData)), localAppData);
            if (!length || length >= sizeof(localAppData))
                return false;
        }

        std::string root = localAppData;
        if (!root.empty() && root.back() != '\\')
            root.push_back('\\');
        root += "Battlezone15Shim";
        if (!EnsureDirectory(root))
            return false;

        std::string movieDir = root + "\\movie_cache";
        if (!EnsureDirectory(movieDir))
            return false;

        HANDLE file = CreateFileA(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        BY_HANDLE_FILE_INFORMATION info = {};
        const BOOL infoOk = GetFileInformationByHandle(file, &info);
        CloseHandle(file);
        if (!infoOk)
            return false;

        std::string base = FileNameOnly(sourcePath);
        SanitizeFileName(base);

        char suffix[96] = {};
        sprintf_s(suffix, "_%08lX%08lX_%08lX%08lX_compat.avi",
                  static_cast<unsigned long>(info.ftLastWriteTime.dwHighDateTime),
                  static_cast<unsigned long>(info.ftLastWriteTime.dwLowDateTime),
                  static_cast<unsigned long>(info.nFileSizeHigh),
                  static_cast<unsigned long>(info.nFileSizeLow));

        cachePath = movieDir + "\\" + base + suffix;
        return true;
    }

    bool FindFfmpeg(std::string& ffmpegPath)
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&g_originalMciSendCommandA), &self))
        {
            char modulePath[4096] = {};
            const DWORD length = GetModuleFileNameA(self, modulePath,
                                                    static_cast<DWORD>(sizeof(modulePath)));
            if (length && length < sizeof(modulePath))
            {
                std::string adjacent(modulePath, length);
                const size_t slash = adjacent.find_last_of("\\/");
                if (slash != std::string::npos)
                {
                    adjacent.resize(slash + 1);
                    adjacent += "ffmpeg.exe";
                    if (FileExistsNonEmpty(adjacent))
                    {
                        ffmpegPath = adjacent;
                        return true;
                    }
                }
            }
        }

        char found[4096] = {};
        const DWORD length = SearchPathA(nullptr, "ffmpeg.exe", nullptr,
                                         static_cast<DWORD>(sizeof(found)), found, nullptr);
        if (!length || length >= sizeof(found))
            return false;

        ffmpegPath.assign(found, length);
        return true;
    }

    std::string QuoteArg(const std::string& value)
    {
        return std::string("\"") + value + "\"";
    }

    bool RunFfmpegConversion(const std::string& sourcePath, const std::string& cachePath)
    {
        std::string ffmpeg;
        if (!FindFfmpeg(ffmpeg))
        {
            ShimLog("movie: IV50 fallback needs ffmpeg.exe next to winmm.dll or available on PATH");
            return false;
        }

        const std::string tempPath = cachePath + ".tmp.avi";
        DeleteFileA(tempPath.c_str());

        std::string command = QuoteArg(ffmpeg) +
            " -hide_banner -loglevel error -nostdin -y -i " + QuoteArg(sourcePath) +
            " -c:v rawvideo -pix_fmt bgr24 -c:a pcm_s16le " + QuoteArg(tempPath);

        std::vector<char> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back('\0');

        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};

        ShimLog("movie: converting IV50 source to compatibility cache: %s", sourcePath.c_str());

        if (!CreateProcessA(ffmpeg.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            ShimLog("movie: ffmpeg launch failed err=%lu", GetLastError());
            return false;
        }

        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        DWORD exitCode = 0xFFFFFFFF;
        if (wait == WAIT_OBJECT_0)
            GetExitCodeProcess(process.hProcess, &exitCode);
        else
            TerminateProcess(process.hProcess, 1);

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (wait != WAIT_OBJECT_0 || exitCode != 0 || !FileExistsNonEmpty(tempPath))
        {
            ShimLog("movie: ffmpeg conversion failed wait=%lu exit=%lu",
                    static_cast<unsigned long>(wait), static_cast<unsigned long>(exitCode));
            DeleteFileA(tempPath.c_str());
            return false;
        }

        DeleteFileA(cachePath.c_str());
        if (!MoveFileExA(tempPath.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            ShimLog("movie: failed to finalize compatibility cache err=%lu", GetLastError());
            DeleteFileA(tempPath.c_str());
            return false;
        }

        ShimLog("movie: compatibility cache ready: %s", cachePath.c_str());
        return true;
    }

    bool EnsureCompatibleAvi(const char* elementName, std::string& cachePath)
    {
        std::string sourcePath;
        if (!ResolveFullPath(elementName, sourcePath))
            return false;

        if (!IsIv50Avi(sourcePath))
            return false;

        if (!BuildCachePath(sourcePath, cachePath))
        {
            ShimLog("movie: could not build IV50 compatibility cache path for %s", sourcePath.c_str());
            return false;
        }

        if (FileExistsNonEmpty(cachePath))
        {
            ShimLog("movie: using existing IV50 compatibility cache: %s", cachePath.c_str());
            return true;
        }

        return RunFfmpegConversion(sourcePath, cachePath);
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        if (!g_originalMciSendCommandA)
            return MCIERR_UNSUPPORTED_FUNCTION;

        MCIERROR result = g_originalMciSendCommandA(deviceId, message, flags, param);

        if (result == 0 || message != MCI_OPEN || !param)
            return result;

        if (!(flags & MCI_OPEN_ELEMENT) || (flags & MCI_OPEN_ELEMENT_ID))
            return result;

        auto* open = reinterpret_cast<MCI_DGV_OPEN_PARMSA*>(param);
        if (!open->lpstrElementName || !*open->lpstrElementName)
            return result;

        std::string cachePath;
        if (!EnsureCompatibleAvi(open->lpstrElementName, cachePath))
            return result;

        // Battlezone opens AVIVideo as a digital-video MCI device and supplies
        // window style/parent fields (MCI_DGV_OPEN_WS / MCI_DGV_OPEN_PARENT).
        // Preserve the full structure on retry; copying only MCI_OPEN_PARMSA
        // drops those fields and causes MCIERR_CREATEWINDOW (347).
        MCI_DGV_OPEN_PARMSA retry = *open;
        retry.wDeviceID = 0;

        // The Win32 digital-video structure declares lpstrElementName as LPSTR,
        // even though MCI treats the open element name as input. Give it a
        // writable, call-scoped buffer rather than casting away constness.
        std::vector<char> retryElement(cachePath.begin(), cachePath.end());
        retryElement.push_back('\0');
        retry.lpstrElementName = retryElement.data();

        ShimLog("movie: retrying failed MCI_OPEN through compatibility cache for %s parent=%p style=%08lX",
                open->lpstrElementName,
                open->hWndParent,
                static_cast<unsigned long>(open->dwStyle));

        const MCIERROR retryResult = g_originalMciSendCommandA(
            deviceId, message, flags, reinterpret_cast<DWORD_PTR>(&retry));

        if (retryResult == 0)
        {
            open->wDeviceID = retry.wDeviceID;
            ShimLog("movie: IV50 compatibility MCI_OPEN succeeded device=%u",
                    static_cast<unsigned int>(retry.wDeviceID));
            return 0;
        }

        ShimLog("movie: compatibility MCI_OPEN failed err=%lu",
                static_cast<unsigned long>(retryResult));
        return result;
    }
}

bool InstallLegacyMovieFix()
{
    if (!EnableWinmmMciMovieProbe())
        return false;

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !HookImport(exe, "winmm.dll", "mciSendCommandA",
                            reinterpret_cast<void*>(&HookMciSendCommandA),
                            reinterpret_cast<void**>(&g_originalMciSendCommandA)))
    {
        ShimLog("movie: could not install MCI IV50 fallback hook");
        DisableWinmmMciMovieProbe();
        return false;
    }

    ShimLog("movie: MCI IV50 compatibility fallback active");
    ShimLog("movie: stock AVI files remain untouched; converted cache is stored under LOCALAPPDATA");
    return true;
}

void ShutdownLegacyMovieFix()
{
    DisableWinmmMciMovieProbe();
}
