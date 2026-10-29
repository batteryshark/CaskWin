param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\graphics-09\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{foreach($bits in @(32,64)){
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath";$temporary=Join-Path $PSScriptRoot "build\texture-thread$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null;$env:TEMP=$temporary;$env:TMP=$temporary
    $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'};if((& "$toolchain\g++.exe" -dumpmachine) -ne $target){throw 'Wrong texture fixture toolchain'}
    foreach($preference in @('default','discrete')){
        [string[]]$options=if($preference -eq 'discrete'){@('-DPREFER_DISCRETE=1')}else{@()}
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode @options "$PSScriptRoot\d3d11-texture-thread-probe.cpp" -o "$OutputDirectory\d3d11-texture-$preference$bits.exe" -luser32 -ldxguid -lopengl32 -lgdi32 -ldbghelp
        if($LASTEXITCODE){throw 'Texture/thread fixture build failed'}
    }
}}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
