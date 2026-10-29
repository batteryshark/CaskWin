param([ValidateSet('all','x64','x86')][string]$Arch='all',[string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$env:TEMP=Join-Path $PSScriptRoot 'build\media-probe-tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $OutputDirectory,$env:TEMP -Force | Out-Null
$savedPath=$env:PATH
try {
    foreach($bits in $(if($Arch -eq 'all'){@(32,64)}elseif($Arch -eq 'x64'){@(64)}else{@(32)})) {
        $toolchain=if($bits -eq 64){'C:\msys64\mingw64\bin'}else{'C:\msys64\mingw32\bin'}
        $env:PATH="$toolchain;$savedPath"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\media-probe.cpp" -o "$OutputDirectory\media-probe$bits.exe"
        if($LASTEXITCODE){throw "Media probe $bits compilation failed"}
    }
} finally {$env:PATH=$savedPath}
