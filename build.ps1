$ErrorActionPreference = 'Stop'

cmake -S . -B build -A Win32
cmake --build build --config Release

Write-Host "Built: $PSScriptRoot\build\Release\winmm.dll"
