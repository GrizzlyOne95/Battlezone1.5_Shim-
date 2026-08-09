#include "winmm_proxy.h"
#include "shim_log.h"

#include <Windows.h>
#include <mmsystem.h>
#include <cstring>

namespace
{
    HMODULE g_realWinmm = nullptr;

#define WINMM_EXPORTS(X) \
    X(CloseDriver) \
    X(DefDriverProc) \
    X(DriverCallback) \
    X(DrvGetModuleHandle) \
    X(GetDriverModuleHandle) \
    X(NotifyCallbackData) \
    X(OpenDriver) \
    X(PlaySoundA) \
    X(PlaySoundW) \
    X(SendDriverMessage) \
    X(WOW32DriverCallback) \
    X(WOW32ResolveMultiMediaHandle) \
    X(WOWAppExit) \
    X(sndPlaySoundA) \
    X(sndPlaySoundW) \
    X(timeBeginPeriod) \
    X(timeEndPeriod) \
    X(timeGetDevCaps) \
    X(timeGetSystemTime) \
    X(timeGetTime) \
    X(timeKillEvent) \
    X(timeSetEvent) \
    X(mmsystemGetVersion) \
    X(waveOutOpen) \
    X(waveOutClose) \
    X(waveOutWrite) \
    X(waveOutPause) \
    X(waveOutRestart) \
    X(waveOutReset) \
    X(waveOutBreakLoop) \
    X(waveOutPrepareHeader) \
    X(waveOutUnprepareHeader) \
    X(waveOutGetPosition) \
    X(waveOutGetPitch) \
    X(waveOutSetPitch) \
    X(waveOutGetPlaybackRate) \
    X(waveOutSetPlaybackRate) \
    X(waveOutGetVolume) \
    X(waveOutSetVolume) \
    X(waveOutGetErrorTextA) \
    X(waveOutGetErrorTextW) \
    X(waveOutGetNumDevs) \
    X(waveOutGetDevCapsA) \
    X(waveOutGetDevCapsW) \
    X(waveOutGetID) \
    X(waveOutMessage) \
    X(waveInOpen) \
    X(waveInClose) \
    X(waveInAddBuffer) \
    X(waveInStart) \
    X(waveInStop) \
    X(waveInReset) \
    X(waveInPrepareHeader) \
    X(waveInUnprepareHeader) \
    X(waveInGetPosition) \
    X(waveInGetErrorTextA) \
    X(waveInGetErrorTextW) \
    X(waveInGetNumDevs) \
    X(waveInGetDevCapsA) \
    X(waveInGetDevCapsW) \
    X(waveInGetID) \
    X(waveInMessage) \
    X(midiOutOpen) \
    X(midiOutClose) \
    X(midiOutShortMsg) \
    X(midiOutLongMsg) \
    X(midiOutPrepareHeader) \
    X(midiOutUnprepareHeader) \
    X(midiOutReset) \
    X(midiOutGetVolume) \
    X(midiOutSetVolume) \
    X(midiOutCacheDrumPatches) \
    X(midiOutCachePatches) \
    X(midiOutGetErrorTextA) \
    X(midiOutGetErrorTextW) \
    X(midiOutGetNumDevs) \
    X(midiOutGetDevCapsA) \
    X(midiOutGetDevCapsW) \
    X(midiOutGetID) \
    X(midiOutMessage) \
    X(midiInOpen) \
    X(midiInClose) \
    X(midiInAddBuffer) \
    X(midiInStart) \
    X(midiInStop) \
    X(midiInReset) \
    X(midiInPrepareHeader) \
    X(midiInUnprepareHeader) \
    X(midiInGetErrorTextA) \
    X(midiInGetErrorTextW) \
    X(midiInGetNumDevs) \
    X(midiInGetDevCapsA) \
    X(midiInGetDevCapsW) \
    X(midiInGetID) \
    X(midiInMessage) \
    X(midiConnect) \
    X(midiDisconnect) \
    X(midiStreamOpen) \
    X(midiStreamClose) \
    X(midiStreamOut) \
    X(midiStreamPause) \
    X(midiStreamPosition) \
    X(midiStreamProperty) \
    X(midiStreamRestart) \
    X(midiStreamStop) \
    X(auxGetNumDevs) \
    X(aux32Message) \
    X(auxGetDevCapsA) \
    X(auxGetDevCapsW) \
    X(auxGetVolume) \
    X(auxSetVolume) \
    X(auxOutMessage) \
    X(mixerGetNumDevs) \
    X(mixerOpen) \
    X(mixerClose) \
    X(mixerMessage) \
    X(mixerGetDevCapsA) \
    X(mixerGetDevCapsW) \
    X(mixerGetID) \
    X(mixerGetLineInfoA) \
    X(mixerGetLineInfoW) \
    X(mixerGetLineControlsA) \
    X(mixerGetLineControlsW) \
    X(mixerGetControlDetailsA) \
    X(mixerGetControlDetailsW) \
    X(mixerSetControlDetails) \
    X(joyGetDevCapsA) \
    X(joy32Message) \
    X(joyGetDevCapsW) \
    X(joyGetNumDevs) \
    X(joyGetPos) \
    X(joyGetPosEx) \
    X(joyGetThreshold) \
    X(joyReleaseCapture) \
    X(joySetCapture) \
    X(joySetThreshold) \
    X(joyConfigChanged) \
    X(mmioOpenA) \
    X(mmioOpenW) \
    X(mmioClose) \
    X(mmioRead) \
    X(mmioWrite) \
    X(mmioSeek) \
    X(mmioGetInfo) \
    X(mmioSetInfo) \
    X(mmioSetBuffer) \
    X(mmioFlush) \
    X(mmioAdvance) \
    X(mmioDescend) \
    X(mmioAscend) \
    X(mmioCreateChunk) \
    X(mmioRenameA) \
    X(mmioRenameW) \
    X(mmioSendMessage) \
    X(mmioInstallIOProcA) \
    X(mmioInstallIOProcW) \
    X(mmioStringToFOURCCA) \
    X(mmioStringToFOURCCW) \
    X(mciSendCommandA) \
    X(mci32Message) \
    X(mciDriverNotify) \
    X(mciDriverYield) \
    X(mciFreeCommandResource) \
    X(mciLoadCommandResource) \
    X(mciSendCommandW) \
    X(mciSendStringA) \
    X(mciSendStringW) \
    X(mciGetErrorStringA) \
    X(mciGetErrorStringW) \
    X(mciSetYieldProc) \
    X(mciGetCreatorTask) \
    X(mciGetYieldProc) \
    X(mciExecute) \
    X(mciGetDeviceIDA) \
    X(mciGetDeviceIDW) \
    X(mciGetDeviceIDFromElementIDA) \
    X(mciGetDeviceIDFromElementIDW) \
    X(mciGetDriverData) \
    X(mciSetDriverData) \
    X(mid32Message) \
    X(mmDrvInstall) \
    X(mmGetCurrentTask) \
    X(mmTaskBlock) \
    X(mmTaskCreate) \
    X(mmTaskSignal) \
    X(mmTaskYield) \
    X(mod32Message) \
    X(mxd32Message) \
    X(tid32Message) \
    X(wid32Message) \
    X(wod32Message)

#define DECLARE_SLOT(name) FARPROC g_##name = nullptr;
    WINMM_EXPORTS(DECLARE_SLOT)
#undef DECLARE_SLOT

    using MciSendCommandAFn = MCIERROR (WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
    using MciSendStringAFn = MCIERROR (WINAPI*)(LPCSTR, LPSTR, UINT, HWND);
    using MciGetErrorStringAFn = BOOL (WINAPI*)(MCIERROR, LPSTR, UINT);

    MciSendCommandAFn g_systemMciSendCommandA = nullptr;
    MciSendStringAFn g_systemMciSendStringA = nullptr;
    MciGetErrorStringAFn g_systemMciGetErrorStringA = nullptr;
    bool g_mciMovieProbeEnabled = false;

    const char* MciMessageName(UINT message)
    {
        switch (message)
        {
        case MCI_OPEN: return "OPEN";
        case MCI_CLOSE: return "CLOSE";
        case MCI_PLAY: return "PLAY";
        case MCI_SEEK: return "SEEK";
        case MCI_STOP: return "STOP";
        case MCI_PAUSE: return "PAUSE";
        case MCI_RESUME: return "RESUME";
        case MCI_STATUS: return "STATUS";
        case MCI_SET: return "SET";
        case MCI_GETDEVCAPS: return "GETDEVCAPS";
        case MCI_INFO: return "INFO";
        case MCI_SYSINFO: return "SYSINFO";
        default: return "OTHER";
        }
    }

    void MciErrorText(MCIERROR error, char text[160])
    {
        text[0] = '\0';
        if (!error)
        {
            strcpy_s(text, 160, "success");
            return;
        }

        if (g_systemMciGetErrorStringA &&
            g_systemMciGetErrorStringA(error, text, 160))
        {
            return;
        }

        sprintf_s(text, 160, "unknown MCI error %lu",
                  static_cast<unsigned long>(error));
    }

    MCIERROR WINAPI HookMciSendCommandA(MCIDEVICEID deviceId, UINT message,
                                        DWORD_PTR flags, DWORD_PTR param)
    {
        const MCIERROR result = g_systemMciSendCommandA
            ? g_systemMciSendCommandA(deviceId, message, flags, param)
            : MCIERR_UNSUPPORTED_FUNCTION;

        if (!g_mciMovieProbeEnabled)
            return result;

        char errorText[160] = {};
        MciErrorText(result, errorText);

        if (message == MCI_OPEN && param)
        {
            auto* open = reinterpret_cast<MCI_OPEN_PARMSA*>(param);
            const char* type = "<unspecified>";
            const char* element = "<unspecified>";

            if ((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID))
                type = open->lpstrDeviceType ? open->lpstrDeviceType : "<null>";
            else if (flags & MCI_OPEN_TYPE_ID)
                type = "<type-id>";

            if ((flags & MCI_OPEN_ELEMENT) && !(flags & MCI_OPEN_ELEMENT_ID))
                element = open->lpstrElementName ? open->lpstrElementName : "<null>";
            else if (flags & MCI_OPEN_ELEMENT_ID)
                element = "<element-id>";

            ShimLog("movie: MCI command OPEN(A) flags=%08lX type=%s element=%s -> err=%lu (%s) device=%u",
                    static_cast<unsigned long>(flags), type, element,
                    static_cast<unsigned long>(result), errorText,
                    static_cast<unsigned int>(open->wDeviceID));
        }
        else
        {
            ShimLog("movie: MCI command %s(A) msg=%04X device=%u flags=%08lX param=%p -> err=%lu (%s)",
                    MciMessageName(message), message,
                    static_cast<unsigned int>(deviceId),
                    static_cast<unsigned long>(flags),
                    reinterpret_cast<void*>(param),
                    static_cast<unsigned long>(result), errorText);
        }

        return result;
    }

    MCIERROR WINAPI HookMciSendStringA(LPCSTR command, LPSTR returnString,
                                       UINT returnChars, HWND callback)
    {
        const MCIERROR result = g_systemMciSendStringA
            ? g_systemMciSendStringA(command, returnString, returnChars, callback)
            : MCIERR_UNSUPPORTED_FUNCTION;

        if (g_mciMovieProbeEnabled)
        {
            char errorText[160] = {};
            MciErrorText(result, errorText);
            ShimLog("movie: MCI string(A) \"%s\" -> err=%lu (%s) return=\"%s\"",
                    command ? command : "<null>",
                    static_cast<unsigned long>(result), errorText,
                    (returnString && returnChars) ? returnString : "");
        }

        return result;
    }

    FARPROC Resolve(const char* name)
    {
        FARPROC proc = GetProcAddress(g_realWinmm, name);
        if (!proc)
            ShimLog("winmm: system export %s was not found", name);
        return proc;
    }
}

bool LoadRealWinmm()
{
    char path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryA(path, MAX_PATH);
    if (!length || length >= MAX_PATH)
    {
        ShimLog("winmm: GetSystemDirectoryA failed err=%lu", GetLastError());
        return false;
    }

    strcat_s(path, "\\winmm.dll");
    g_realWinmm = LoadLibraryA(path);
    if (!g_realWinmm)
    {
        ShimLog("winmm: failed to load %s err=%lu", path, GetLastError());
        return false;
    }

#define RESOLVE_SLOT(name) g_##name = Resolve(#name);
    WINMM_EXPORTS(RESOLVE_SLOT)
#undef RESOLVE_SLOT

    g_systemMciSendCommandA = reinterpret_cast<MciSendCommandAFn>(g_mciSendCommandA);
    g_systemMciSendStringA = reinterpret_cast<MciSendStringAFn>(g_mciSendStringA);
    g_systemMciGetErrorStringA = reinterpret_cast<MciGetErrorStringAFn>(g_mciGetErrorStringA);

    ShimLog("winmm: loaded real System32 winmm.dll at %p", g_realWinmm);
    return true;
}

bool EnableWinmmMciMovieProbe()
{
    if (!g_realWinmm || !g_systemMciSendCommandA || !g_systemMciSendStringA)
    {
        ShimLog("movie: MCI probe unavailable because required System32 WinMM exports were not resolved");
        return false;
    }

    g_mciSendCommandA = reinterpret_cast<FARPROC>(&HookMciSendCommandA);
    g_mciSendStringA = reinterpret_cast<FARPROC>(&HookMciSendStringA);
    g_mciMovieProbeEnabled = true;

    ShimLog("movie: WinMM MCI probe active (ANSI command + string APIs)");
    return true;
}

void DisableWinmmMciMovieProbe()
{
    g_mciMovieProbeEnabled = false;

    if (g_systemMciSendCommandA)
        g_mciSendCommandA = reinterpret_cast<FARPROC>(g_systemMciSendCommandA);
    if (g_systemMciSendStringA)
        g_mciSendStringA = reinterpret_cast<FARPROC>(g_systemMciSendStringA);
}

void FreeRealWinmm()
{
    if (g_realWinmm)
    {
        FreeLibrary(g_realWinmm);
        g_realWinmm = nullptr;
    }
}

#if !defined(_M_IX86)
#error Battlezone 1.5 shim must be built for Win32/x86.
#endif

#define DECLARE_STUB(name) \
    extern "C" __declspec(naked) void __cdecl proxy_##name() \
    { \
        __asm { jmp dword ptr [g_##name] } \
    }

WINMM_EXPORTS(DECLARE_STUB)
#undef DECLARE_STUB
#undef WINMM_EXPORTS
