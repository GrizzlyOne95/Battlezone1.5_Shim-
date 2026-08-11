// The shim's logger writes into the game directory; the self-test just prints.

#include <cstdarg>
#include <cstdio>

void ShimLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fputs("  [shim] ", stdout);
    std::vprintf(format, args);
    std::fputc('\n', stdout);
    va_end(args);
}

extern "C" void IndeoLogLine(const char* text)
{
    std::printf("  [decoder] %s", text);
}
