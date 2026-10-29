param([ValidateSet('x86','x64','all')][string]$Arch='all')
$ErrorActionPreference='Stop'
$output=Join-Path $PSScriptRoot 'bin'
$temporary=Join-Path $PSScriptRoot 'build\cdda-probe'
New-Item -ItemType Directory -Force -Path $output,$temporary|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    $env:TEMP=$temporary;$env:TMP=$temporary
    foreach($target in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
        $bits=if($target -eq 'x86'){32}else{64}
        $compiler=if($target -eq 'x86'){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $env:Path="$compiler;$savedPath"
        & "$compiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -static -static-libgcc -static-libstdc++ -municode "$PSScriptRoot\cdda-probe.cpp" -o "$output\cdda-probe$bits.exe" -lwinmm -luser32
        if($LASTEXITCODE){throw "CDDA fixture build failed: $target"}
    }
} finally {$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
