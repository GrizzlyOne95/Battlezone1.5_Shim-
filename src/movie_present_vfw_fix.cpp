#include "movie_present_vfw_fix.h"
#include "shim_log.h"

#include <Windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <vfw.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using MciSendCommandAFn = MCIERROR (WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
    using AVIFileInitFn = void (WINAPI*)();
    using AVIFileExitFn = void (WINAPI*)();
    using AVIStreamOpenFromFileAFn = HRESULT (WINAPI*)(PAVISTREAM*, LPCSTR, DWORD, LONG, UINT, CLSID*);
    using AVIStreamGetFrameOpenFn = PGETFRAME (WINAPI*)(PAVISTREAM, LPBITMAPINFOHEADER);
    using AVIStreamGetFrameFn = LPVOID (WINAPI*)(PGETFRAME, LONG);
    using AVIStreamGetFrameCloseFn = HRESULT (WINAPI*)(PGETFRAME);
    using AVIStreamTimeToSampleFn = LONG (WINAPI*)(PAVISTREAM, LONG);

    MciSendCommandAFn g_nextMciSendCommandA = nullptr;

    HMODULE g_avifil32 = nullptr;
    AVIFileInitFn g_aviFileInit = nullptr;
    AVIFileExitFn g_aviFileExit = nullptr;
    AVIStreamOpenFromFileAFn g_aviStreamOpenFromFileA = nullptr;
    AVIStreamGetFrameOpenFn g_aviStreamGetFrameOpen = nullptr;
    AVIStreamGetFrameFn g_aviStreamGetFrame = nullptr;
    AVIStreamGetFrameCloseFn g_aviStreamGetFrameClose = nullptr;
    AVIStreamTimeToSampleFn g_aviStreamTimeToSample = nullptr;
    bool g_aviInitialized = false;

    constexpr UINT kFrameIntervalMs = 33;
    constexpr UINT_PTR kTimerBase = 0xB350;
    constexpr size_t kMaxMovieSlots = 16;

    struct MovieSlot
    {
        MCIDEVICEID deviceId = 0;
        HWND mciWindow = nullptr;
        RECT destination = {};
        bool haveDestination = false;
        bool playing = false;
        bool classified = false;
        bool useVfw = false;
        bool vfwFailed = false;
        bool firstPaintLogged = false;
        UINT_PTR timerId = 0;
        std::string effectivePath;

        // Scratch surface used only to ask MCIAVI whether it can actually supply
        // pixels for this device. A successful all-black update is the signal to
        // switch the device to direct VfW frame extraction.
        HDC memoryDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        void* bits = nullptr;
        int surfaceW = 0;
        int surfaceH = 0;

        PAVISTREAM stream = nullptr;
        PGETFRAME getFrame = nullptr;
        DWORD timeFormat = MCI_FORMAT_MILLISECONDS;
        bool timeFormatKnown = false;
    };

    std::array<MovieSlot, kMaxMovieSlots> g_slots = {};

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

    bool HookImport(HMODULE module, const char* importedDll, const char* importedName,
                    void* replacement, void** original)
    {
        if (!module)
            return false;

        const auto base = reinterpret_cast<std::uint8_t*>(module);
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
                if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), importedName) != 0)
                    continue;

                void** slot = reinterpret_cast<void**>(&firstThunk->u1.Function);
                return PatchPointer(slot, replacement, original);
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

        if (!readOk || bytesRead < 12 ||
            std::memcmp(data.data(), "RIFF", 4) != 0 ||
            std::memcmp(data.data() + 8, "AVI ", 4) != 0)
        {
            return false;
        }

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
        sprintf_s(suffix, "_%08lX%08lX_%08lX%08lX_compat_v2.avi",
                  static_cast<unsigned long>(info.ftLastWriteTime.dwHighDateTime),
                  static_cast<unsigned long>(info.ftLastWriteTime.dwLowDateTime),
                  static_cast<unsigned long>(info.nFileSizeHigh),
                  static_cast<unsigned long>(info.nFileSizeLow));

        std::string root = localAppData;
        if (!root.empty() && root.back() != '\\')
            root.push_back('\\');
        cachePath = root + "Battlezone15Shim\\movie_cache\\" + base + suffix;
        return true;
    }

    bool ResolveEffectiveMoviePath(const char* element, std::string& effectivePath)
    {
        std::string sourcePath;
        if (!ResolveFullPath(element, sourcePath))
            return false;

        if (!IsIv50Avi(sourcePath))
        {
            effectivePath = sourcePath;
            return FileExistsNonEmpty(effectivePath);
        }

        std::string cachePath;
        if (!BuildCachePath(sourcePath, cachePath) || !FileExistsNonEmpty(cachePath))
            return false;

        effectivePath = cachePath;
        return true;
    }

    bool LoadVfw()
    {
        if (g_avifil32)
            return true;

        g_avifil32 = LoadLibraryA("avifil32.dll");
        if (!g_avifil32)
        {
            ShimLog("movievfw: could not load avifil32.dll err=%lu", GetLastError());
            return false;
        }

#define RESOLVE_VFW(name) \
        g_##name = reinterpret_cast<name##Fn>(GetProcAddress(g_avifil32, #name)); \
        if (!g_##name) { ShimLog("movievfw: missing avifil32 export %s", #name); return false; }

        RESOLVE_VFW(aviFileInit);
        RESOLVE_VFW(aviFileExit);
        RESOLVE_VFW(aviStreamOpenFromFileA);
        RESOLVE_VFW(aviStreamGetFrameOpen);
        RESOLVE_VFW(aviStreamGetFrame);
        RESOLVE_VFW(aviStreamGetFrameClose);
        RESOLVE_VFW(aviStreamTimeToSample);
#undef RESOLVE_VFW

        g_aviFileInit();
        g_aviInitialized = true;
        ShimLog("movievfw: Avifil32 direct-frame backend ready");
        return true;
    }

    MovieSlot* FindSlot(MCIDEVICEID id)
    {
        if (!id)
            return nullptr;
        for (auto& slot : g_slots)
        {
            if (slot.deviceId == id)
                return &slot;
        }
        return nullptr;
    }

    MovieSlot* FindOrAllocateSlot(MCIDEVICEID id)
    {
        if (MovieSlot* existing = FindSlot(id))
            return existing;

        for (auto& slot : g_slots)
        {
            if (!slot.deviceId)
            {
                slot = {};
                slot.deviceId = id;
                return &slot;
            }
        }
        return nullptr;
    }

    void StopTimer(MovieSlot& slot)
    {
        slot.playing = false;
        if (slot.timerId)
            KillTimer(nullptr, slot.timerId);
        slot.timerId = 0;
    }

    void ReleaseScratch(MovieSlot& slot)
    {
        if (slot.memoryDc)
        {
            if (slot.oldBitmap)
                SelectObject(slot.memoryDc, slot.oldBitmap);
            if (slot.dib)
                DeleteObject(slot.dib);
            DeleteDC(slot.memoryDc);
        }
        slot.memoryDc = nullptr;
        slot.dib = nullptr;
        slot.oldBitmap = nullptr;
        slot.bits = nullptr;
        slot.surfaceW = 0;
        slot.surfaceH = 0;
    }

    void ReleaseVfw(MovieSlot& slot)
    {
        if (slot.getFrame && g_aviStreamGetFrameClose)
            g_aviStreamGetFrameClose(slot.getFrame);
        slot.getFrame = nullptr;

        if (slot.stream)
            slot.stream->Release();
        slot.stream = nullptr;
    }

    void ReleaseSlot(MCIDEVICEID id)
    {
        if (MovieSlot* slot = FindSlot(id))
        {
            StopTimer(*slot);
            ReleaseScratch(*slot);
            ReleaseVfw(*slot);
            *slot = {};
        }
    }

    bool EnsureScratch(MovieSlot& slot)
    {
        if (!slot.mciWindow || !IsWindow(slot.mciWindow))
            return false;

        RECT client = {};
        if (!GetClientRect(slot.mciWindow, &client))
            return false;

        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
            return false;

        if (slot.memoryDc && slot.surfaceW == width && slot.surfaceH == height)
            return true;

        ReleaseScratch(slot);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        slot.memoryDc = CreateCompatibleDC(screen);
        slot.dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &slot.bits, nullptr, 0);
        ReleaseDC(nullptr, screen);

        if (!slot.memoryDc || !slot.dib || !slot.bits)
        {
            ReleaseScratch(slot);
            return false;
        }

        slot.oldBitmap = SelectObject(slot.memoryDc, slot.dib);
        slot.surfaceW = width;
        slot.surfaceH = height;
        return true;
    }

    bool DestinationHasPixels(const MovieSlot& slot)
    {
        if (!slot.bits || !slot.haveDestination || slot.surfaceW <= 0 || slot.surfaceH <= 0)
            return false;

        const int x0 = max(0, static_cast<int>(slot.destination.left));
        const int y0 = max(0, static_cast<int>(slot.destination.top));
        const int x1 = min(slot.surfaceW, x0 + max(0, static_cast<int>(slot.destination.right)));
        const int y1 = min(slot.surfaceH, y0 + max(0, static_cast<int>(slot.destination.bottom)));
        if (x1 <= x0 || y1 <= y0)
            return false;

        const auto* pixels = static_cast<const std::uint32_t*>(slot.bits);
        for (int y = y0; y < y1; ++y)
        {
            const auto* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(slot.surfaceW);
            for (int x = x0; x < x1; ++x)
            {
                if ((row[x] & 0x00FFFFFFu) != 0)
                    return true;
            }
        }
        return false;
    }

    HWND FindVisibleAncestor(HWND hwnd)
    {
        HWND current = hwnd;
        while (current)
        {
            if (IsWindowVisible(current))
                return current;
            current = GetParent(current);
        }
        return nullptr;
    }

    bool MapDestination(const MovieSlot& slot, HWND paintWindow, POINT& point,
                        int& width, int& height)
    {
        if (!slot.mciWindow || !paintWindow || !slot.haveDestination)
            return false;

        point.x = slot.destination.left;
        point.y = slot.destination.top;
        width = static_cast<int>(slot.destination.right);
        height = static_cast<int>(slot.destination.bottom);
        if (width <= 0 || height <= 0)
            return false;

        SetLastError(ERROR_SUCCESS);
        const int result = MapWindowPoints(slot.mciWindow, paintWindow, &point, 1);
        if (!result && GetLastError() != ERROR_SUCCESS)
            return false;
        return true;
    }

    bool PaintMciFrame(MovieSlot& slot)
    {
        if (!EnsureScratch(slot))
            return false;

        std::memset(slot.bits, 0, static_cast<size_t>(slot.surfaceW) *
                                  static_cast<size_t>(slot.surfaceH) * 4u);

        MCI_DGV_UPDATE_PARMS update = {};
        update.hDC = slot.memoryDc;
        const MCIERROR updateResult = g_nextMciSendCommandA(
            slot.deviceId,
            MCI_UPDATE,
            MCI_DGV_UPDATE_HDC | MCI_DGV_UPDATE_PAINT,
            reinterpret_cast<DWORD_PTR>(&update));
        if (updateResult != 0)
            return false;

        if (!DestinationHasPixels(slot))
            return false;

        HWND paintWindow = FindVisibleAncestor(slot.mciWindow);
        if (!paintWindow)
            return false;

        POINT point = {};
        int width = 0;
        int height = 0;
        if (!MapDestination(slot, paintWindow, point, width, height))
            return false;

        HDC out = GetDC(paintWindow);
        if (!out)
            return false;

        const BOOL ok = BitBlt(out, point.x, point.y, width, height,
                               slot.memoryDc,
                               static_cast<int>(slot.destination.left),
                               static_cast<int>(slot.destination.top),
                               SRCCOPY);
        ReleaseDC(paintWindow, out);

        if (!slot.firstPaintLogged)
        {
            ShimLog("movievfw: device=%u MCI supplied pixels; keeping native MCI presentation dest=%ld,%ld %dx%d blit=%d",
                    static_cast<unsigned int>(slot.deviceId),
                    slot.destination.left, slot.destination.top,
                    width, height, ok ? 1 : 0);
            slot.firstPaintLogged = true;
        }
        return ok != FALSE;
    }

    bool EnsureVfwDecoder(MovieSlot& slot)
    {
        if (slot.getFrame && slot.stream)
            return true;
        if (slot.vfwFailed || slot.effectivePath.empty() || !LoadVfw())
            return false;

        HRESULT hr = g_aviStreamOpenFromFileA(
            &slot.stream,
            slot.effectivePath.c_str(),
            streamtypeVIDEO,
            0,
            OF_READ,
            nullptr);
        if (FAILED(hr) || !slot.stream)
        {
            ShimLog("movievfw: AVIStreamOpenFromFileA failed device=%u hr=%08lX path=%s",
                    static_cast<unsigned int>(slot.deviceId),
                    static_cast<unsigned long>(hr), slot.effectivePath.c_str());
            slot.stream = nullptr;
            slot.vfwFailed = true;
            return false;
        }

        slot.getFrame = g_aviStreamGetFrameOpen(slot.stream, nullptr);
        if (!slot.getFrame)
        {
            ShimLog("movievfw: AVIStreamGetFrameOpen failed device=%u path=%s",
                    static_cast<unsigned int>(slot.deviceId), slot.effectivePath.c_str());
            slot.stream->Release();
            slot.stream = nullptr;
            slot.vfwFailed = true;
            return false;
        }

        MCI_STATUS_PARMS status = {};
        status.dwItem = MCI_STATUS_TIME_FORMAT;
        const MCIERROR statusResult = g_nextMciSendCommandA(
            slot.deviceId, MCI_STATUS, MCI_STATUS_ITEM,
            reinterpret_cast<DWORD_PTR>(&status));
        if (statusResult == 0)
        {
            slot.timeFormat = static_cast<DWORD>(status.dwReturn);
            slot.timeFormatKnown = true;
        }

        ShimLog("movievfw: direct frame decoder ready device=%u timeFormat=%lu path=%s",
                static_cast<unsigned int>(slot.deviceId),
                static_cast<unsigned long>(slot.timeFormat),
                slot.effectivePath.c_str());
        return true;
    }

    LONG CurrentSample(MovieSlot& slot)
    {
        MCI_STATUS_PARMS status = {};
        status.dwItem = MCI_STATUS_POSITION;
        const MCIERROR result = g_nextMciSendCommandA(
            slot.deviceId, MCI_STATUS, MCI_STATUS_ITEM,
            reinterpret_cast<DWORD_PTR>(&status));
        if (result != 0)
            return -1;

        const LONG position = static_cast<LONG>(status.dwReturn);
        if (slot.timeFormatKnown && slot.timeFormat == MCI_FORMAT_FRAMES)
            return position;

        // MCIAVI normally reports milliseconds unless explicitly changed to
        // frames. AVIStreamTimeToSample gives the exact corresponding video
        // sample for the stream rate, so 15/29.97/etc. are handled correctly.
        if (!slot.timeFormatKnown || slot.timeFormat == MCI_FORMAT_MILLISECONDS)
            return g_aviStreamTimeToSample(slot.stream, position);

        return -1;
    }

    const BYTE* PackedDibBits(const BITMAPINFOHEADER* header)
    {
        if (!header || header->biSize < sizeof(BITMAPINFOHEADER))
            return nullptr;

        size_t offset = header->biSize;
        if (header->biBitCount <= 8)
        {
            const DWORD colors = header->biClrUsed
                ? header->biClrUsed
                : (1u << header->biBitCount);
            offset += static_cast<size_t>(colors) * sizeof(RGBQUAD);
        }
        else if (header->biCompression == BI_BITFIELDS &&
                 header->biSize == sizeof(BITMAPINFOHEADER))
        {
            offset += 3u * sizeof(DWORD);
        }

        return reinterpret_cast<const BYTE*>(header) + offset;
    }

    bool PaintVfwFrame(MovieSlot& slot)
    {
        if (!EnsureVfwDecoder(slot))
            return false;

        const LONG sample = CurrentSample(slot);
        if (sample < 0)
            return false;

        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(
            g_aviStreamGetFrame(slot.getFrame, sample));
        if (!header)
            return false;

        const BYTE* frameBits = PackedDibBits(header);
        if (!frameBits || header->biWidth == 0 || header->biHeight == 0)
            return false;

        HWND paintWindow = FindVisibleAncestor(slot.mciWindow);
        if (!paintWindow)
            return false;

        POINT point = {};
        int width = 0;
        int height = 0;
        if (!MapDestination(slot, paintWindow, point, width, height))
            return false;

        HDC out = GetDC(paintWindow);
        if (!out)
            return false;

        SetStretchBltMode(out, COLORONCOLOR);
        const int sourceW = header->biWidth < 0 ? -header->biWidth : header->biWidth;
        const int sourceH = header->biHeight < 0 ? -header->biHeight : header->biHeight;
        const int copied = StretchDIBits(
            out,
            point.x, point.y, width, height,
            0, 0, sourceW, sourceH,
            frameBits,
            reinterpret_cast<const BITMAPINFO*>(header),
            DIB_RGB_COLORS,
            SRCCOPY);
        ReleaseDC(paintWindow, out);

        if (!slot.firstPaintLogged)
        {
            ShimLog("movievfw: device=%u direct frame sample=%ld src=%dx%d bpp=%u compression=%08lX dest=%ld,%ld %dx%d copied=%d",
                    static_cast<unsigned int>(slot.deviceId),
                    sample, sourceW, sourceH,
                    static_cast<unsigned int>(header->biBitCount),
                    static_cast<unsigned long>(header->biCompression),
                    slot.destination.left, slot.destination.top,
                    width, height, copied);
            slot.firstPaintLogged = true;
        }

        return copied != GDI_ERROR && copied != 0;
    }

    bool PaintCurrentFrame(MovieSlot& slot)
    {
        if (!slot.playing || !slot.mciWindow || !slot.haveDestination ||
            !IsWindow(slot.mciWindow))
        {
            return false;
        }

        if (!slot.classified)
        {
            if (PaintMciFrame(slot))
            {
                slot.classified = true;
                slot.useVfw = false;
                return true;
            }

            slot.classified = true;
            slot.useVfw = true;
            slot.firstPaintLogged = false;
            ShimLog("movievfw: device=%u MCI_UPDATE produced no usable pixels; switching to direct VfW frames",
                    static_cast<unsigned int>(slot.deviceId));
        }

        return slot.useVfw ? PaintVfwFrame(slot) : PaintMciFrame(slot);
    }

    void CALLBACK PaintTimerProc(HWND, UINT, UINT_PTR timerId, DWORD)
    {
        for (auto& slot : g_slots)
        {
            if (slot.timerId == timerId && slot.playing)
            {
                PaintCurrentFrame(slot);
                return;
            }
        }
    }

    void StartTimer(MovieSlot& slot)
    {
        if (!slot.mciWindow || !slot.haveDestination)
            return;

        slot.playing = true;
        if (slot.timerId)
            return;

        const UINT_PTR requested = kTimerBase + static_cast<UINT_PTR>(slot.deviceId);
        slot.timerId = SetTimer(nullptr, requested, kFrameIntervalMs, &PaintTimerProc);
        if (!slot.timerId)
        {
            slot.playing = false;
            ShimLog("movievfw: SetTimer failed device=%u err=%lu",
                    static_cast<unsigned int>(slot.deviceId), GetLastError());
        }
        else
        {
            ShimLog("movievfw: deferred presenter active device=%u interval=%ums",
                    static_cast<unsigned int>(slot.deviceId), kFrameIntervalMs);
        }
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        if (!g_nextMciSendCommandA)
            return MCIERR_UNSUPPORTED_FUNCTION;

        if (message == MCI_CLOSE)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StopTimer(*slot);
        }

        const MCIERROR result = g_nextMciSendCommandA(deviceId, message, flags, param);
        if (result != 0)
            return result;

        if (message == MCI_OPEN && param)
        {
            auto* open = reinterpret_cast<MCI_OPEN_PARMSA*>(param);
            MovieSlot* slot = FindOrAllocateSlot(open->wDeviceID);
            if (slot)
            {
                slot->effectivePath.clear();
                const char* element = nullptr;
                if ((flags & MCI_OPEN_ELEMENT) && !(flags & MCI_OPEN_ELEMENT_ID))
                    element = open->lpstrElementName;

                if (element && ResolveEffectiveMoviePath(element, slot->effectivePath))
                {
                    ShimLog("movievfw: device=%u effective AVI path=%s",
                            static_cast<unsigned int>(open->wDeviceID),
                            slot->effectivePath.c_str());
                }
                else
                {
                    ShimLog("movievfw: device=%u no direct-frame path resolved for %s",
                            static_cast<unsigned int>(open->wDeviceID),
                            element ? element : "<unknown>");
                }
            }
        }
        else if (message == MCI_WINDOW && param && (flags & MCI_DGV_WINDOW_HWND))
        {
            auto* window = reinterpret_cast<MCI_DGV_WINDOW_PARMSA*>(param);
            if (MovieSlot* slot = FindOrAllocateSlot(deviceId))
            {
                if (slot->mciWindow != window->hWnd)
                {
                    StopTimer(*slot);
                    ReleaseScratch(*slot);
                    slot->mciWindow = window->hWnd;
                    slot->classified = false;
                    slot->useVfw = false;
                    slot->firstPaintLogged = false;
                }
            }
        }
        else if (message == MCI_PUT && param &&
                 (flags & MCI_DGV_RECT) && (flags & MCI_DGV_PUT_DESTINATION))
        {
            auto* put = reinterpret_cast<MCI_DGV_PUT_PARMS*>(param);
            if (MovieSlot* slot = FindOrAllocateSlot(deviceId))
            {
                slot->destination = put->rc;
                slot->haveDestination = true;
                slot->classified = false;
                slot->firstPaintLogged = false;
            }
        }
        else if (message == MCI_PLAY)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StartTimer(*slot);
        }
        else if (message == MCI_SEEK)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                slot->firstPaintLogged = false;
        }
        else if (message == MCI_STOP || message == MCI_PAUSE)
        {
            if (MovieSlot* slot = FindSlot(deviceId))
                StopTimer(*slot);
        }
        else if (message == MCI_CLOSE)
        {
            ReleaseSlot(deviceId);
        }

        return result;
    }
}

bool InstallMovieVfwPresentationFix()
{
    if (!LoadVfw())
    {
        ShimLog("movievfw: direct-frame presentation backend unavailable");
        return false;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !HookImport(exe, "winmm.dll", "mciSendCommandA",
                            reinterpret_cast<void*>(&HookMciSendCommandA),
                            reinterpret_cast<void**>(&g_nextMciSendCommandA)))
    {
        ShimLog("movievfw: could not install direct-frame MCI presentation hook");
        return false;
    }

    ShimLog("movievfw: adaptive MCI/VfW movie presentation fallback active");
    return true;
}

void ShutdownMovieVfwPresentationFix()
{
    for (auto& slot : g_slots)
    {
        if (slot.deviceId)
        {
            StopTimer(slot);
            ReleaseScratch(slot);
            ReleaseVfw(slot);
        }
        slot = {};
    }

    if (g_aviInitialized && g_aviFileExit)
        g_aviFileExit();
    g_aviInitialized = false;

    if (g_avifil32)
        FreeLibrary(g_avifil32);
    g_avifil32 = nullptr;
    g_aviFileInit = nullptr;
    g_aviFileExit = nullptr;
    g_aviStreamOpenFromFileA = nullptr;
    g_aviStreamGetFrameOpen = nullptr;
    g_aviStreamGetFrame = nullptr;
    g_aviStreamGetFrameClose = nullptr;
    g_aviStreamTimeToSample = nullptr;
}
