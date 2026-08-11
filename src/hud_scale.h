#pragma once

// Scales the in-game HUD so it stays readable at high resolutions.
// Returns false when the fix is disabled by bz15_shim.ini or could not be
// installed, in which case the game is left completely stock.
bool InstallHudScale();
void ShutdownHudScale();
