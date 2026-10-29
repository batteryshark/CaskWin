param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    foreach($bits in @(32,64)){
        $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
        $temporary=Join-Path $PSScriptRoot "build\system-loader-probe$bits"
        New-Item -ItemType Directory -Force -Path $temporary|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $compiler=Join-Path $toolchain 'g++.exe'
        $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $target){throw 'Unexpected system loader fixture compiler architecture'}
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++')
        & $compiler @flags -municode "$PSScriptRoot\system-loader-probe.cpp" -o (Join-Path $OutputDirectory "system-loader-probe$bits.exe")
        if($LASTEXITCODE){throw 'System loader probe build failed'}
        & $compiler @flags -shared -DWINEDOWS_SYSTEM_LOADER_MARKER "$PSScriptRoot\system-loader-probe.cpp" '-Wl,--kill-at' -o (Join-Path $OutputDirectory "system-loader-marker$bits.dll")
        if($LASTEXITCODE){throw 'Benign system loader marker build failed'}
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
