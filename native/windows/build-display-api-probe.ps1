param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\display-api-09\fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$temp=Join-Path $OutputDirectory 'tmp';New-Item -ItemType Directory -Force -Path $temp|Out-Null
$saved=@{path=$env:Path;temp=$env:TEMP;tmp=$env:TMP}
try{
    $env:TEMP=$temp;$env:TMP=$temp
    foreach($bits in @(32,64)){
        $toolchain="C:\msys64\mingw$bits\bin";$env:Path="$toolchain;$($saved.path)"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'display-api-probe.cpp') -luser32 -o (Join-Path $OutputDirectory "display-api-probe$bits.exe")
        if($LASTEXITCODE){throw "Display API fixture build failed: $bits"}
        $imports=& "$toolchain\objdump.exe" -p (Join-Path $OutputDirectory "display-api-probe$bits.exe")
        if($imports -match '(?i)DLL Name:.*(msys-|libgcc|libstdc\+\+|libwinpthread)'){throw 'Unexpected compiler runtime import'}
    }
}finally{$env:Path=$saved.path;$env:TEMP=$saved.temp;$env:TMP=$saved.tmp}
$global:LASTEXITCODE=0
