// Routes the vendored decoder's av_log output into the shim's log file.
// The validation tool in tools/indeo_check supplies its own copy of this, which
// is why it lives outside src/indeo.

#include "shim_log.h"

extern "C" void IndeoLogLine(const char* text)
{
    // The decoder's messages end in a newline; ShimLog adds its own.
    char trimmed[512];
    size_t length = 0;

    while (text[length] && length < sizeof(trimmed) - 1)
    {
        ++length;
    }
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
    {
        --length;
    }

    for (size_t i = 0; i < length; ++i)
    {
        trimmed[i] = text[i];
    }
    trimmed[length] = '\0';

    ShimLog("indeo: %s", trimmed);
}
