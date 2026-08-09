#include "winmm_proxy.h"
#include "shim_log.h"

#include <Windows.h>
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

    ShimLog("winmm: loaded real System32 winmm.dll at %p", g_realWinmm);
    return true;
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
