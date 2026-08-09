$ErrorActionPreference = 'Stop'

cmake -S . -B build -A Win32
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

cmake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE"
}

$dll = Join-Path $PSScriptRoot 'build\Release\winmm.dll'
if (-not (Test-Path $dll)) {
    throw "Build completed without producing $dll"
}

Write-Host "Built: $dll"
