param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$env:TEMP=Join-Path $OutputDirectory 'compiler-tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $OutputDirectory,$env:TEMP -Force|Out-Null
$savedPath=$env:PATH
try {
    foreach($bits in @(32,64)) {
        $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\legacy-registry-probe.cpp" -ladvapi32 -o "$OutputDirectory\legacy-registry-probe$bits.exe"
        if($LASTEXITCODE){throw "Legacy registry probe $bits compilation failed"}
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -shared -static -static-libgcc -static-libstdc++ -D_WIN32_WINNT=0x0A00 "$PSScriptRoot\legacy-registry-module.cpp" -ladvapi32 -o "$OutputDirectory\legacy-registry-module$bits.dll"
        if($LASTEXITCODE){throw "Legacy registry module $bits compilation failed"}
    }
}finally{$env:PATH=$savedPath}
