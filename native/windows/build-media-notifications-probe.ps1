param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$env:TEMP=Join-Path $PSScriptRoot 'build\media-notifications-probe-tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $OutputDirectory,$env:TEMP -Force|Out-Null
# MSVC accepts wstring streams directly; MinGW requires filesystem::path. Adapt
# only the standalone fixture's build copy, preserving the production sources.
$common=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'common.cpp')
$common=$common.Replace('#include "common.h"','#include "common.h"'+[Environment]::NewLine+'#include <filesystem>')
$common=[regex]::Replace($common,'std::([io])fstream (\w+)\(([^,\r\n]+),','std::$1fstream $2(std::filesystem::path($3),')
$commonCopy=Join-Path $env:TEMP 'common-notifications.cpp';[IO.File]::WriteAllText($commonCopy,$common)
$savedPath=$env:PATH
try{
    foreach($bits in @(32,64)){
        $toolchain=if($bits -eq 64){'C:\msys64\mingw64\bin'}else{'C:\msys64\mingw32\bin'}
        $env:PATH="$toolchain;$savedPath"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\media-notifications-probe.cpp" -luser32 -o "$OutputDirectory\media-notifications-probe$bits.exe"
        if($LASTEXITCODE){throw "Media notification probe $bits compilation failed"}
        # LTO and section collection keep unused prefix bootstrap/registry code
        # out of this standalone fixture; media state and observer are production.
        $sources=@('media-notifications-service-probe.cpp','media-notifications.cpp','media-state.cpp','media-descriptor.cpp','cdda-media.cpp')|ForEach-Object {Join-Path $PSScriptRoot $_}
        & "$toolchain\g++.exe" -std=c++17 -O2 -flto -Wall -Wextra -Wno-misleading-indentation -municode -static -static-libgcc -static-libstdc++ -ffunction-sections -fdata-sections '-Wl,--gc-sections' -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -I $PSScriptRoot @sources $commonCopy -luser32 -ladvapi32 -o "$OutputDirectory\media-notifications-service-probe$bits.exe"
        if($LASTEXITCODE){throw "Media notification service probe $bits compilation failed"}
    }
}finally{$env:PATH=$savedPath}
