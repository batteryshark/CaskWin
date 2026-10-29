param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\disabled-alias-fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{foreach($bits in @(32,64)){
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
    $build=Join-Path $PSScriptRoot "build\disabled-alias-$bits";New-Item -ItemType Directory -Force -Path $build,$OutputDirectory|Out-Null
    $env:TEMP=$build;$env:TMP=$build
    $compiler=Join-Path $toolchain 'g++.exe';$expected=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& $compiler -dumpmachine) -ne $expected){throw "Unexpected compiler target for $bits"}
    $output=Join-Path $OutputDirectory "disabled-alias-probe$bits.exe"
    & $compiler -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ -municode (Join-Path $PSScriptRoot 'disabled-alias-probe.cpp') -o $output
    if($LASTEXITCODE){throw "Alias probe build failed for $bits"}
    $imports=& (Join-Path $toolchain 'objdump.exe') -p $output|Select-String 'DLL Name:'
    if($LASTEXITCODE -or $imports -match 'libgcc_s|libstdc\+\+|libwinpthread'){throw "Unexpected probe imports for $bits"}
    Write-Host "Built alias probe $bits ($expected)"
}}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
