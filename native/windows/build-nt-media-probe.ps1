param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    $env:TEMP=Join-Path $PSScriptRoot 'build\nt-media-probe';$env:TMP=$env:TEMP
    New-Item -ItemType Directory -Force -Path $OutputDirectory,$env:TEMP|Out-Null
    foreach($bits in @(32,64)){
        $toolchain=if($bits -eq 64){'C:\msys64\mingw64\bin'}else{'C:\msys64\mingw32\bin'}
        $env:Path="$toolchain;$savedPath"
        $expected=if($bits -eq 64){'x86_64-w64-mingw32'}else{'i686-w64-mingw32'}
        if((& "$toolchain\g++.exe" -dumpmachine) -ne $expected){throw 'Wrong NT probe compiler architecture'}
        $output=Join-Path $OutputDirectory "nt-media-probe$bits.exe"
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0602 "$PSScriptRoot\nt-media-probe.cpp" -o $output
        if($LASTEXITCODE){throw "NT media probe $bits compilation failed"}
        if((& "$toolchain\objdump.exe" -p $output|Select-String 'DLL Name') -match 'libgcc|libstdc\+\+|libwinpthread'){throw 'Unexpected external compiler runtime import'}
        Write-Host "Built $output"
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
