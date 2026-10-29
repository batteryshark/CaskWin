param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    $compiler='C:\msys64\mingw64\bin\g++.exe';$env:Path='C:\msys64\mingw64\bin;'+$savedPath
    $temporary=Join-Path $PSScriptRoot 'build\game-audio-timing';New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $env:TEMP=$temporary;$env:TMP=$temporary
    if((& $compiler -dumpmachine) -ne 'x86_64-w64-mingw32'){throw 'Expected native x64 MinGW compiler'}
    $output=Join-Path $OutputDirectory 'game-audio-timing-probe64.exe'
    & $compiler -std=c++17 -O2 -g -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -static -static-libgcc -static-libstdc++ -municode -DUNICODE -D_UNICODE '-D_WIN32_WINNT=0x0602' "$PSScriptRoot\game-audio-timing-probe.cpp" "$PSScriptRoot\game-audio-wasapi.cpp" "$PSScriptRoot\game-audio-render.cpp" -o $output -lole32 -luuid
    if($LASTEXITCODE){throw 'Game audio timing fixture compilation failed'}
    $imports=& C:\msys64\mingw64\bin\objdump.exe -p $output|Select-String 'DLL Name'
    if($imports -match 'libgcc|libstdc\+\+|libwinpthread|SDL2|FAudio'){throw 'Diagnostic unexpectedly imports an external runtime instead of resolving copied audio DLLs'}
    Write-Host "Built $output"
    $waveOutput=Join-Path $OutputDirectory 'game-audio-waveout-probe64.exe'
    & $compiler -std=c++17 -O2 -g -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode '-D_WIN32_WINNT=0x0602' "$PSScriptRoot\game-audio-waveout-probe.cpp" -o $waveOutput -lwinmm
    if($LASTEXITCODE){throw 'Native WinMM control compilation failed'}
    $waveImports=& C:\msys64\mingw64\bin\objdump.exe -p $waveOutput|Select-String 'DLL Name'
    if($waveImports -match 'libgcc|libstdc\+\+|libwinpthread|SDL2|FAudio'){throw 'Native WinMM control imports an unexpected audio/runtime library'}
    Write-Host "Built $waveOutput"
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
