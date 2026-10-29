param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$env:TEMP=Join-Path $PSScriptRoot 'build\media-metadata-tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $OutputDirectory,$env:TEMP -Force|Out-Null
$savedPath=$env:PATH
try{
    foreach($bits in @(32,64)){
        $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\media-metadata-probe.cpp" -o "$OutputDirectory\media-metadata-probe$bits.exe"
        if($LASTEXITCODE){throw "Media metadata probe $bits compilation failed"}
    }
}finally{$env:PATH=$savedPath}
