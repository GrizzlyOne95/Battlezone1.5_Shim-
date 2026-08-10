#pragma once

// Registers an in-process Indeo Video 5 decompressor with Video for Windows.
bool InstallVideoCodecShim();
void ShutdownVideoCodecShim();
