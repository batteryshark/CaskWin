param([ValidateSet('x86','x64','all')][string]$Arch='all')
$ErrorActionPreference='Stop'
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
$output=Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force -Path $output|Out-Null
try {
    foreach($a in $arches){
        $bits=if($a -eq 'x86'){'32'}else{'64'}
        $toolchain="C:\msys64\mingw$bits\bin"
        $compiler=Join-Path $toolchain 'g++.exe'
        if(!(Test-Path -LiteralPath $compiler)){throw "Install the MSYS2 mingw$bits GCC toolchain to build the DirectInput fixtures."}
        $env:Path="$toolchain;$savedPath"
        $temporary=Join-Path $PSScriptRoot "build\input-probe\$a"
        New-Item -ItemType Directory -Force -Path $temporary|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $target=& $compiler -dumpmachine
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if($LASTEXITCODE -or $target -ne $expected){throw "Unexpected input fixture compiler target: $target"}
        # Modern Windows SDKs omit the legacy DirectInput import library.
        # MinGW supplies both APIs and the standard data-format objects.
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++','-municode','-DUNICODE','-D_UNICODE')
        $libraries=@('-ldinput','-ldinput8','-ldxguid','-lole32','-luser32')
        $source=Join-Path $PSScriptRoot 'input-probe.cpp'
        & $compiler @flags $source -o (Join-Path $output "input-probe$bits.exe") @libraries
        if($LASTEXITCODE){throw "Dynamic DirectInput fixture build failed ($a)"}
        & $compiler @flags -DWINEDOWS_STATIC_INPUT_IMPORTS $source -o (Join-Path $output "input-import-probe$bits.exe") @libraries
        if($LASTEXITCODE){throw "Static DirectInput fixture build failed ($a)"}
        Write-Host "DirectInput fixtures built: $a"
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
