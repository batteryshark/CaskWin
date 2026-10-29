param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\session-fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
        $bits=if($a -eq 'x86'){32}else{64}
        $toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
        if(!(Test-Path -LiteralPath $compiler)){throw "Missing MinGW $bits compiler"}
        $env:Path="$toolchain;$savedPath"
        $temp=Join-Path $OutputDirectory "build-$a";New-Item -ItemType Directory -Force -Path $temp|Out-Null
        $env:TEMP=$temp;$env:TMP=$temp
        $machine=& $compiler -dumpmachine
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if($LASTEXITCODE -or $machine -ne $expected){throw "Unexpected compiler target: $machine"}
        & $compiler -std=c++17 -O2 -Wall -Wextra -Werror -municode -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'session-probe.cpp') -o (Join-Path $OutputDirectory "session-probe$bits.exe") -ladvapi32
        if($LASTEXITCODE){throw "Session probe compilation failed: $a"}
        & $compiler -std=c++17 -O2 -Wall -Wextra -Werror -municode -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'session-job-probe.cpp') -o (Join-Path $OutputDirectory "session-job-probe$bits.exe") -ladvapi32
        if($LASTEXITCODE){throw "Session lifecycle probe compilation failed: $a"}
    }
} finally {$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
