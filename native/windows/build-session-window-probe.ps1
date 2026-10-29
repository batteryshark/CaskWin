param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\session-window-fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$oldPath=$env:Path;$oldTemp=$env:TEMP;$oldTmp=$env:TMP
try{
    foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
        $bits=if($a -eq 'x86'){32}else{64};$tools="C:\msys64\mingw$bits\bin";$env:Path="$tools;$oldPath"
        $temporary=Join-Path $OutputDirectory "build-$a";New-Item -ItemType Directory -Force -Path $temporary|Out-Null;$env:TEMP=$temporary;$env:TMP=$temporary
        & "$tools\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation -municode -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'session-window-probe.cpp') -o (Join-Path $OutputDirectory "session-window-probe$bits.exe") -luser32 -lgdi32
        if($LASTEXITCODE){throw "Window session fixture failed to compile: $a"}
    }
}finally{$env:Path=$oldPath;$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
$global:LASTEXITCODE=0
