param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\display-08\fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {foreach($bits in @(32,64)){
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
    $temporary=Join-Path $PSScriptRoot "build\desktop-monitor$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $env:TEMP=$temporary;$env:TMP=$temporary
    $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& "$toolchain\g++.exe" -dumpmachine) -ne $target){throw 'Wrong desktop-monitor fixture architecture'}
    & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -static -static-libgcc -static-libstdc++ -municode "$PSScriptRoot\desktop-monitor-probe.cpp" -o (Join-Path $OutputDirectory "desktop-monitor-probe$bits.exe") -luser32 -lgdi32
    if($LASTEXITCODE){throw 'Desktop monitor fixture build failed'}
}} finally {$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
