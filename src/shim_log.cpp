#include "shim_log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
    SRWLOCK g_logLock = SRWLOCK_INIT;

    void BuildLogPath(char (&path)[MAX_PATH])
    {
        path[0] = '\0';
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
        {
            strcpy_s(path, "bz15_shim.log");
            return;
        }

        char* slash = strrchr(path, '\\');
        if (slash)
        {
            slash[1] = '\0';
            strcat_s(path, "bz15_shim.log");
        }
        else
        {
            strcpy_s(path, "bz15_shim.log");
        }
    }
}

void ShimLog(const char* format, ...)
{
    char message[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char line[2300] = {};
    _snprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        message);

    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&g_logLock);

    char path[MAX_PATH] = {};
    BuildLogPath(path);
    HANDLE file = CreateFileA(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
    }

    ReleaseSRWLockExclusive(&g_logLock);
}
