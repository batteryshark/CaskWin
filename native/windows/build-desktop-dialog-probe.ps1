param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-04'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{foreach($bits in @(32,64)){
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
    $temporary=Join-Path $PSScriptRoot "build\desktop-dialog$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $env:TEMP=$temporary;$env:TMP=$temporary
    $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& "$toolchain\g++.exe" -dumpmachine) -ne $target){throw 'Wrong desktop fixture architecture'}
    & "$toolchain\windres.exe" "$PSScriptRoot\desktop-dialog-probe.rc" -O coff -o "$temporary\resource.o"
    if($LASTEXITCODE){throw 'Desktop dialog resource failed'}
    & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode "$PSScriptRoot\desktop-dialog-probe.cpp" "$temporary\resource.o" -o (Join-Path $OutputDirectory "desktop-dialog-probe$bits.exe") -luser32 -lgdi32
    if($LASTEXITCODE){throw 'Desktop dialog fixture failed'}
}}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
