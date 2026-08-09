#include "movie_fix.h"
#include "shim_log.h"
#include "winmm_proxy.h"

bool InstallLegacyMovieFix()
{
    if (!EnableWinmmMciMovieProbe())
        return false;

    ShimLog("movie: probing Battlezone AnimButton through the WinMM/MCI proxy path");
    return true;
}

void ShutdownLegacyMovieFix()
{
    DisableWinmmMciMovieProbe();
}
