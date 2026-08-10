// movie_fix.cpp
//
// Battlezone 1.5 plays every menu animation and every cutscene through MCI's
// "AVIVideo" device (mciavi32.dll), which decodes video through Video for
// Windows. movie_open, AnimButton::movie_open and the intro/outro/credits paths
// all funnel into mciSendCommandA(MCI_OPEN).
//
// Almost all of the shipped AVIs are Indeo Video 5 (fccHandler "IV50"):
//
//     movie\intro.avi     320x240  IV50 + audio
//     movie\outro.avi     320x240  IV50 + audio
//     movie\credits.avi   640x480  IV50 + audio
//     anims\*.avi         mostly   IV50, video only
//
// Microsoft removed the Indeo decoders from Windows years ago and never
// replaced them, so on a modern machine there is no ir50_32.dll and nothing
// else that can decode IV50. The remaining handful of clips -- posters.avi,
// sspil.avi, svhra.avi (Microsoft Video 1, "msvc") and bzone.avi (uncompressed)
// -- still play, which is exactly why the symptom looks like "some of the menu
// videos are broken" rather than "video is broken".
//
// The two failure shapes differ, and the difference matters:
//
//   * anims\*.avi hold a single video stream. With no decompressor for it
//     MCI_OPEN itself fails (MCIERR_INVALID_DEVICE_NAME-ish, error 6).
//
//   * movie\*.avi hold video *and* audio. MCI_OPEN succeeds, MCI_PLAY succeeds,
//     the position advances and the audio is audible -- and not a single frame
//     is ever drawn. Nothing reports an error at any point.
//
// So a fallback that only reacts to a failed MCI_OPEN can never fix the
// cutscenes. This one instead inspects the file up front: it reads the AVI
// header, pulls the video stream's fccHandler/biCompression, and asks Video for
// Windows whether a decompressor for it actually exists. Only when the answer
// is no does it substitute a transcoded copy.
//
// The transcode target is Microsoft Video 1, which is the codec posters.avi
// already uses and which therefore is proven to decode on the target machine.
// It also encodes in well under a second per clip, where the previous
// uncompressed-DIB target would have produced a ~400 MB intro.avi.
//
// Stock game files are never modified; converted copies live under LOCALAPPDATA.
// The cache path is kept deliberately short because MCI rejects long element
// paths outright with error 304 ("the filename is invalid").

#include "movie_fix.h"
#include "shim_log.h"
#include "winmm_proxy.h"

#include <Windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <vfw.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    using MciSendCommandAFn = MCIERROR (WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);

    MciSendCommandAFn g_originalMciSendCommandA = nullptr;

    // Resolved element name -> substitute path. An empty value means "this file
    // is fine, pass it through", which keeps the menu from re-probing the same
    // clip every time the player moves the highlight.
    std::map<std::string, std::string> g_substitutions;
    SRWLOCK g_substitutionLock = SRWLOCK_INIT;

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

        char buffer[MAX_PATH * 2] = {};
        const DWORD length = GetFullPathNameA(path, static_cast<DWORD>(sizeof(buffer)), buffer, nullptr);
        if (!length || length >= sizeof(buffer))
            return false;

        fullPath.assign(buffer, length);
        return true;
    }

    // ---- AVI header inspection ---------------------------------------------

    struct AviVideoFormat
    {
        DWORD handler = 0;      // strh.fccHandler, e.g. 'IV50' / 'msvc'
        DWORD compression = 0;  // strf.biCompression, e.g. 'IV50' / 'CRAM' / BI_RGB
        LONG width = 0;
        LONG height = 0;
        bool valid = false;
    };

    DWORD ReadU32(const unsigned char* p)
    {
        return static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8) |
               (static_cast<DWORD>(p[2]) << 16) | (static_cast<DWORD>(p[3]) << 24);
    }

    constexpr DWORD FourCc(char a, char b, char c, char d)
    {
        return static_cast<DWORD>(static_cast<unsigned char>(a)) |
               (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
               (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
    }

    void FourCcToText(DWORD value, char (&text)[5])
    {
        for (int i = 0; i < 4; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>((value >> (i * 8)) & 0xFF);
            text[i] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '.';
        }
        text[4] = '\0';
    }

    // Pull the first video stream's format out of the 'hdrl' list. The AVI
    // header always sits at the front of the file, so a bounded read is enough
    // and we never have to page in a 17 MB cutscene to classify it.
    bool ReadAviVideoFormat(const std::string& path, AviVideoFormat& out)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        constexpr DWORD kHeaderBytes = 64 * 1024;
        std::vector<unsigned char> data(kHeaderBytes);
        DWORD read = 0;
        const BOOL readOk = ReadFile(file, data.data(), kHeaderBytes, &read, nullptr);
        CloseHandle(file);

        if (!readOk || read < 32)
            return false;

        if (ReadU32(data.data()) != FourCc('R', 'I', 'F', 'F') ||
            ReadU32(data.data() + 8) != FourCc('A', 'V', 'I', ' '))
        {
            return false;
        }

        // Locate the hdrl list among the top-level chunks.
        DWORD hdrlBegin = 0;
        DWORD hdrlEnd = 0;
        for (DWORD off = 12; off + 12 <= read;)
        {
            const DWORD id = ReadU32(data.data() + off);
            const DWORD size = ReadU32(data.data() + off + 4);
            if (id == FourCc('L', 'I', 'S', 'T') &&
                ReadU32(data.data() + off + 8) == FourCc('h', 'd', 'r', 'l'))
            {
                hdrlBegin = off + 12;
                hdrlEnd = off + 8 + size;
                break;
            }

            const DWORD advance = 8 + size + (size & 1);
            if (advance <= 8 || off + advance <= off)
                break;
            off += advance;
        }

        if (!hdrlBegin)
            return false;
        if (hdrlEnd > read)
            hdrlEnd = read;

        // Walk the stream lists and keep the first one that describes video.
        for (DWORD off = hdrlBegin; off + 12 <= hdrlEnd;)
        {
            const DWORD id = ReadU32(data.data() + off);
            const DWORD size = ReadU32(data.data() + off + 4);

            if (id == FourCc('L', 'I', 'S', 'T') &&
                ReadU32(data.data() + off + 8) == FourCc('s', 't', 'r', 'l'))
            {
                DWORD strlEnd = off + 8 + size;
                if (strlEnd > hdrlEnd)
                    strlEnd = hdrlEnd;

                bool isVideo = false;
                AviVideoFormat candidate;

                for (DWORD inner = off + 12; inner + 8 <= strlEnd;)
                {
                    const DWORD innerId = ReadU32(data.data() + inner);
                    const DWORD innerSize = ReadU32(data.data() + inner + 4);
                    const DWORD body = inner + 8;

                    if (innerId == FourCc('s', 't', 'r', 'h') && innerSize >= 8 &&
                        body + 8 <= strlEnd)
                    {
                        isVideo = ReadU32(data.data() + body) == FourCc('v', 'i', 'd', 's');
                        candidate.handler = ReadU32(data.data() + body + 4);
                    }
                    else if (innerId == FourCc('s', 't', 'r', 'f') && innerSize >= 20 &&
                             body + 20 <= strlEnd)
                    {
                        candidate.width = static_cast<LONG>(ReadU32(data.data() + body + 4));
                        candidate.height = static_cast<LONG>(ReadU32(data.data() + body + 8));
                        candidate.compression = ReadU32(data.data() + body + 16);
                    }

                    const DWORD advance = 8 + innerSize + (innerSize & 1);
                    if (advance <= 8 || inner + advance <= inner)
                        break;
                    inner += advance;
                }

                if (isVideo)
                {
                    candidate.valid = true;
                    out = candidate;
                    return true;
                }
            }

            const DWORD advance = 8 + size + (size & 1);
            if (advance <= 8 || off + advance <= off)
                break;
            off += advance;
        }

        return false;
    }

    // Does this machine actually have something that can decode the stream?
    //
    // ICOpen matches on the *driver* FourCC, which is what strh.fccHandler
    // carries ("msvc", "mrle"). biCompression is often an alias the installer
    // registered under a different name -- Microsoft Video 1 files say "CRAM"
    // there and ICOpen("CRAM") finds nothing even though the codec is present --
    // so the handler has to be probed first and the alias only as a backstop.
    bool VideoDecompressorAvailable(const AviVideoFormat& format)
    {
        // BI_RGB / BI_RLE8 / BI_RLE4 / BI_BITFIELDS need no decompressor at all;
        // MCIAVI hands those straight to GDI.
        if (format.compression <= BI_BITFIELDS)
            return true;

        const DWORD candidates[] = { format.handler, format.compression };
        for (DWORD fourcc : candidates)
        {
            if (!fourcc || fourcc == FourCc('D', 'I', 'B', ' ') ||
                fourcc == FourCc('R', 'A', 'W', ' '))
            {
                return true;
            }

            HIC decompressor = ICOpen(ICTYPE_VIDEO, fourcc, ICMODE_DECOMPRESS);
            if (decompressor)
            {
                ICClose(decompressor);
                return true;
            }
        }

        return false;
    }

    // ---- compatibility cache ------------------------------------------------

    bool EnsureDirectory(const std::string& path)
    {
        if (CreateDirectoryA(path.c_str(), nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    // MCI refuses element paths beyond roughly 128 characters with error 304,
    // so the cache name is a hash rather than anything descriptive. Keep the
    // directory shallow for the same reason.
    bool BuildCachePath(const std::string& sourcePath, std::string& cachePath)
    {
        char localAppData[MAX_PATH * 2] = {};
        DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", localAppData,
                                               static_cast<DWORD>(sizeof(localAppData)));
        if (!length || length >= sizeof(localAppData))
        {
            length = GetTempPathA(static_cast<DWORD>(sizeof(localAppData)), localAppData);
            if (!length || length >= sizeof(localAppData))
                return false;
        }

        std::string root(localAppData, length);
        if (!root.empty() && root.back() != '\\')
            root.push_back('\\');
        root += "bz15shim";
        if (!EnsureDirectory(root))
            return false;

        const std::string movieDir = root + "\\mv";
        if (!EnsureDirectory(movieDir))
            return false;

        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExA(sourcePath.c_str(), GetFileExInfoStandard, &info))
            return false;

        // FNV-1a over the identity of the source, so an updated or replaced AVI
        // produces a different cache entry instead of silently reusing a stale
        // conversion.
        std::uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](const void* bytes, size_t count) {
            const auto* p = static_cast<const unsigned char*>(bytes);
            for (size_t i = 0; i < count; ++i)
            {
                hash ^= p[i];
                hash *= 1099511628211ULL;
            }
        };
        for (char ch : sourcePath)
        {
            const char lower = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            mix(&lower, 1);
        }
        mix(&info.ftLastWriteTime, sizeof(info.ftLastWriteTime));
        mix(&info.nFileSizeLow, sizeof(info.nFileSizeLow));
        mix(&info.nFileSizeHigh, sizeof(info.nFileSizeHigh));

        char name[32] = {};
        sprintf_s(name, "\\%016llX.avi", static_cast<unsigned long long>(hash));

        cachePath = movieDir + name;

        // Anything close to the MCI limit is worse than useless: the game would
        // trade a blank video for a hard open failure.
        if (cachePath.size() > 110)
        {
            ShimLog("movie: compatibility cache path is too long for MCI (%zu chars)",
                    cachePath.size());
            return false;
        }

        return true;
    }

    bool FindFfmpeg(std::string& ffmpegPath)
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&g_originalMciSendCommandA), &self))
        {
            char modulePath[MAX_PATH * 2] = {};
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

        char found[MAX_PATH * 2] = {};
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
            ShimLog("movie: no decoder for this clip and no ffmpeg.exe next to winmm.dll or on PATH");
            return false;
        }

        const std::string tempPath = cachePath + ".t";
        DeleteFileA(tempPath.c_str());

        // Microsoft Video 1 is the one lossy codec the stock game already relies
        // on (posters.avi, sspil.avi, svhra.avi), so it is decodable by
        // definition on any machine that can play those. rgb555le is the only
        // pixel format its encoder accepts. Audio is re-encoded rather than
        // dropped: the cutscenes carry a soundtrack.
        const std::string command = QuoteArg(ffmpeg) +
            " -hide_banner -loglevel error -nostdin -y -i " + QuoteArg(sourcePath) +
            " -c:v msvideo1 -pix_fmt rgb555le -c:a pcm_s16le -f avi " + QuoteArg(tempPath);

        std::vector<char> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back('\0');

        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};

        ShimLog("movie: transcoding to Microsoft Video 1: %s", sourcePath.c_str());

        if (!CreateProcessA(ffmpeg.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            ShimLog("movie: ffmpeg launch failed err=%lu", GetLastError());
            return false;
        }

        // A 640x480 cutscene converts in seconds, but a cold disk and a slow
        // machine deserve room; the alternative is a permanently blank video.
        const DWORD wait = WaitForSingleObject(process.hProcess, 180000);
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

    // Returns true and fills `substitute` when the game should be handed a
    // converted copy instead of the file it asked for.
    bool ResolveSubstitute(const char* elementName, std::string& substitute)
    {
        std::string sourcePath;
        if (!ResolveFullPath(elementName, sourcePath))
            return false;

        // movie_open probes two CD-relative paths before the installed one, so
        // most calls land here on a file that does not exist.
        if (!FileExistsNonEmpty(sourcePath))
            return false;

        {
            AcquireSRWLockShared(&g_substitutionLock);
            const auto found = g_substitutions.find(sourcePath);
            const bool known = found != g_substitutions.end();
            if (known)
                substitute = found->second;
            ReleaseSRWLockShared(&g_substitutionLock);
            if (known)
                return !substitute.empty();
        }

        auto remember = [&sourcePath](const std::string& value) {
            AcquireSRWLockExclusive(&g_substitutionLock);
            g_substitutions[sourcePath] = value;
            ReleaseSRWLockExclusive(&g_substitutionLock);
        };

        AviVideoFormat format;
        if (!ReadAviVideoFormat(sourcePath, format))
        {
            // Not an AVI we understand -- MCI is still free to make sense of it.
            remember(std::string());
            return false;
        }

        if (VideoDecompressorAvailable(format))
        {
            remember(std::string());
            return false;
        }

        char handlerText[5] = {};
        char compressionText[5] = {};
        FourCcToText(format.handler, handlerText);
        FourCcToText(format.compression, compressionText);
        ShimLog("movie: no decompressor for %s (handler=%s compression=%s %ldx%ld)",
                sourcePath.c_str(), handlerText, compressionText, format.width, format.height);

        std::string cachePath;
        if (!BuildCachePath(sourcePath, cachePath))
        {
            remember(std::string());
            return false;
        }

        if (!FileExistsNonEmpty(cachePath) && !RunFfmpegConversion(sourcePath, cachePath))
        {
            // Remember the failure too. Retrying a doomed conversion on every
            // menu highlight would stall the shell repeatedly.
            remember(std::string());
            return false;
        }

        remember(cachePath);
        substitute = cachePath;
        return true;
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        if (!g_originalMciSendCommandA)
            return MCIERR_UNSUPPORTED_FUNCTION;

        const bool isFileOpen = message == MCI_OPEN && param &&
                                (flags & MCI_OPEN_ELEMENT) && !(flags & MCI_OPEN_ELEMENT_ID);
        if (!isFileOpen)
            return g_originalMciSendCommandA(deviceId, message, flags, param);

        auto* open = reinterpret_cast<MCI_DGV_OPEN_PARMSA*>(param);
        if (!open->lpstrElementName || !*open->lpstrElementName)
            return g_originalMciSendCommandA(deviceId, message, flags, param);

        std::string substitute;
        if (!ResolveSubstitute(open->lpstrElementName, substitute))
            return g_originalMciSendCommandA(deviceId, message, flags, param);

        // Battlezone opens AVIVideo as a digital-video device and supplies
        // window style/parent fields (MCI_DGV_OPEN_WS / MCI_DGV_OPEN_PARENT).
        // Preserve the full structure; copying only MCI_OPEN_PARMSA drops those
        // fields and causes MCIERR_CREATEWINDOW (347).
        MCI_DGV_OPEN_PARMSA retry = *open;
        retry.wDeviceID = 0;

        // The Win32 digital-video structure declares lpstrElementName as LPSTR
        // even though MCI treats it as input, so give it a writable, call-scoped
        // buffer rather than casting away constness.
        std::vector<char> element(substitute.begin(), substitute.end());
        element.push_back('\0');
        retry.lpstrElementName = element.data();

        const MCIERROR result = g_originalMciSendCommandA(
            deviceId, message, flags, reinterpret_cast<DWORD_PTR>(&retry));

        if (result == 0)
        {
            open->wDeviceID = retry.wDeviceID;
            return 0;
        }

        ShimLog("movie: compatibility open failed err=%lu; falling back to the stock file",
                static_cast<unsigned long>(result));
        return g_originalMciSendCommandA(deviceId, message, flags, param);
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
        ShimLog("movie: could not install the MCI codec compatibility hook");
        DisableWinmmMciMovieProbe();
        return false;
    }

    ShimLog("movie: MCI codec compatibility active (stock AVI files are never modified;"
            " converted copies live under LOCALAPPDATA\\bz15shim\\mv)");
    return true;
}

void ShutdownLegacyMovieFix()
{
    DisableWinmmMciMovieProbe();
}
