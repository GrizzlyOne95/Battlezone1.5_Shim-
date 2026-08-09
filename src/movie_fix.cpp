#include "movie_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <vfw.h>
#include <cstring>

namespace
{
    using AVIFileOpenAFn = HRESULT (WINAPI*)(PAVIFILE*, LPCSTR, UINT, LPCLSID);
    using AVIStreamOpenFromFileAFn = HRESULT (WINAPI*)(PAVISTREAM*, LPCSTR, DWORD, LONG, UINT, LPCLSID);
    using AVIFileGetStreamFn = HRESULT (WINAPI*)(PAVIFILE, PAVISTREAM*, DWORD, LONG);
    using AVIStreamGetFrameOpenFn = PGETFRAME (WINAPI*)(PAVISTREAM, LPBITMAPINFOHEADER);
    using AVIStreamInfoAFn = HRESULT (WINAPI*)(PAVISTREAM, AVISTREAMINFOA*, LONG);
    using AVIStreamReadFormatFn = HRESULT (WINAPI*)(PAVISTREAM, LONG, LPVOID, LONG*);

    AVIFileOpenAFn g_realAVIFileOpenA = nullptr;
    AVIStreamOpenFromFileAFn g_realAVIStreamOpenFromFileA = nullptr;
    AVIFileGetStreamFn g_realAVIFileGetStream = nullptr;
    AVIStreamGetFrameOpenFn g_realAVIStreamGetFrameOpen = nullptr;

    AVIStreamInfoAFn g_aviStreamInfoA = nullptr;
    AVIStreamReadFormatFn g_aviStreamReadFormat = nullptr;

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

    void FourCCToText(DWORD value, char text[5])
    {
        for (int i = 0; i < 4; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>((value >> (i * 8)) & 0xFF);
            text[i] = (ch >= 32 && ch <= 126) ? static_cast<char>(ch) : '.';
        }
        text[4] = '\0';
    }

    void LogStreamFormat(PAVISTREAM stream)
    {
        if (!stream)
            return;

        AVISTREAMINFOA info = {};
        info.fccType = 0;
        info.fccHandler = 0;

        HRESULT infoHr = E_FAIL;
        if (g_aviStreamInfoA)
            infoHr = g_aviStreamInfoA(stream, &info, sizeof(info));

        unsigned char formatBuffer[256] = {};
        LONG formatSize = sizeof(formatBuffer);
        HRESULT formatHr = E_FAIL;
        if (g_aviStreamReadFormat)
            formatHr = g_aviStreamReadFormat(stream, 0, formatBuffer, &formatSize);

        char typeText[5] = "....";
        char handlerText[5] = "....";
        char compressionText[5] = "....";
        FourCCToText(info.fccType, typeText);
        FourCCToText(info.fccHandler, handlerText);

        if (SUCCEEDED(formatHr) && formatSize >= static_cast<LONG>(sizeof(BITMAPINFOHEADER)))
        {
            const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(formatBuffer);
            FourCCToText(bih->biCompression, compressionText);
            ShimLog("movie: stream info hr=%08X type=%s handler=%s rate=%lu/%lu length=%lu; format hr=%08X compression=%s size=%ld %ldx%ld bpp=%u",
                    static_cast<unsigned int>(infoHr), typeText, handlerText,
                    info.dwRate, info.dwScale, info.dwLength,
                    static_cast<unsigned int>(formatHr), compressionText, formatSize,
                    bih->biWidth, bih->biHeight, bih->biBitCount);
        }
        else
        {
            ShimLog("movie: stream info hr=%08X type=%s handler=%s; format hr=%08X size=%ld",
                    static_cast<unsigned int>(infoHr), typeText, handlerText,
                    static_cast<unsigned int>(formatHr), formatSize);
        }
    }

    HRESULT WINAPI HookAVIFileOpenA(PAVIFILE* file, LPCSTR path, UINT mode, LPCLSID handler)
    {
        const HRESULT hr = g_realAVIFileOpenA
            ? g_realAVIFileOpenA(file, path, mode, handler)
            : E_FAIL;

        ShimLog("movie: AVIFileOpenA path=%s mode=%08X hr=%08X",
                path ? path : "<null>", mode, static_cast<unsigned int>(hr));
        return hr;
    }

    HRESULT WINAPI HookAVIStreamOpenFromFileA(PAVISTREAM* stream, LPCSTR path,
                                               DWORD fccType, LONG index,
                                               UINT mode, LPCLSID handler)
    {
        const HRESULT hr = g_realAVIStreamOpenFromFileA
            ? g_realAVIStreamOpenFromFileA(stream, path, fccType, index, mode, handler)
            : E_FAIL;

        char typeText[5] = "....";
        FourCCToText(fccType, typeText);
        ShimLog("movie: AVIStreamOpenFromFileA path=%s type=%s index=%ld mode=%08X hr=%08X",
                path ? path : "<null>", typeText, index, mode, static_cast<unsigned int>(hr));

        if (SUCCEEDED(hr) && stream && *stream)
            LogStreamFormat(*stream);
        return hr;
    }

    HRESULT WINAPI HookAVIFileGetStream(PAVIFILE file, PAVISTREAM* stream,
                                        DWORD fccType, LONG index)
    {
        const HRESULT hr = g_realAVIFileGetStream
            ? g_realAVIFileGetStream(file, stream, fccType, index)
            : E_FAIL;

        char typeText[5] = "....";
        FourCCToText(fccType, typeText);
        ShimLog("movie: AVIFileGetStream type=%s index=%ld hr=%08X",
                typeText, index, static_cast<unsigned int>(hr));

        if (SUCCEEDED(hr) && stream && *stream)
            LogStreamFormat(*stream);
        return hr;
    }

    PGETFRAME WINAPI HookAVIStreamGetFrameOpen(PAVISTREAM stream, LPBITMAPINFOHEADER wanted)
    {
        PGETFRAME result = g_realAVIStreamGetFrameOpen
            ? g_realAVIStreamGetFrameOpen(stream, wanted)
            : nullptr;

        if (!result)
        {
            ShimLog("movie: AVIStreamGetFrameOpen FAILED wanted=%s",
                    wanted ? "explicit-format" : "default-RGB");
            LogStreamFormat(stream);
        }
        else
        {
            ShimLog("movie: AVIStreamGetFrameOpen succeeded wanted=%s",
                    wanted ? "explicit-format" : "default-RGB");
        }

        return result;
    }

    void ResolveInspectionFunctions()
    {
        HMODULE avifil = GetModuleHandleA("avifil32.dll");
        if (!avifil)
        {
            ShimLog("movie: avifil32.dll is not loaded; format inspection unavailable");
            return;
        }

        g_aviStreamInfoA = reinterpret_cast<AVIStreamInfoAFn>(
            GetProcAddress(avifil, "AVIStreamInfoA"));
        g_aviStreamReadFormat = reinterpret_cast<AVIStreamReadFormatFn>(
            GetProcAddress(avifil, "AVIStreamReadFormat"));
    }
}

bool InstallLegacyMovieFix()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe)
        return false;

    ResolveInspectionFunctions();

    unsigned int hooks = 0;
    if (HookImport(exe, "avifil32.dll", "AVIFileOpenA",
                   reinterpret_cast<void*>(&HookAVIFileOpenA),
                   reinterpret_cast<void**>(&g_realAVIFileOpenA)))
    {
        ++hooks;
        ShimLog("movie: AVIFileOpenA hook installed");
    }

    if (HookImport(exe, "avifil32.dll", "AVIStreamOpenFromFileA",
                   reinterpret_cast<void*>(&HookAVIStreamOpenFromFileA),
                   reinterpret_cast<void**>(&g_realAVIStreamOpenFromFileA)))
    {
        ++hooks;
        ShimLog("movie: AVIStreamOpenFromFileA hook installed");
    }

    if (HookImport(exe, "avifil32.dll", "AVIFileGetStream",
                   reinterpret_cast<void*>(&HookAVIFileGetStream),
                   reinterpret_cast<void**>(&g_realAVIFileGetStream)))
    {
        ++hooks;
        ShimLog("movie: AVIFileGetStream hook installed");
    }

    if (HookImport(exe, "avifil32.dll", "AVIStreamGetFrameOpen",
                   reinterpret_cast<void*>(&HookAVIStreamGetFrameOpen),
                   reinterpret_cast<void**>(&g_realAVIStreamGetFrameOpen)))
    {
        ++hooks;
        ShimLog("movie: AVIStreamGetFrameOpen hook installed");
    }

    if (!hooks)
    {
        ShimLog("movie: no AVI/VfW imports found in bzone.exe; movie hook inactive");
        return false;
    }

    ShimLog("movie: legacy AVI compatibility probe active (%u hooks)", hooks);
    return true;
}

void ShutdownLegacyMovieFix()
{
    // The process is already shutting down. Restoring bzone.exe's IAT here is
    // unnecessary and can race teardown of the imported multimedia DLLs.
}
