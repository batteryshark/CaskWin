param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$env:TEMP=Join-Path $OutputDirectory 'compiler-tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $OutputDirectory,$env:TEMP -Force|Out-Null
$savedPath=$env:PATH
try {
    foreach($bits in @(32,64)) {
        $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
        & "$toolchain\g++.exe" -dumpmachine
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\legacy-launch-probe.cpp" -o "$OutputDirectory\legacy-launch-probe$bits.exe"
        if($LASTEXITCODE){throw "Legacy launch probe $bits compilation failed"}
        & "$toolchain\objdump.exe" -p "$OutputDirectory\legacy-launch-probe$bits.exe" | Select-String 'DLL Name'
    }
}finally{$env:PATH=$savedPath}
