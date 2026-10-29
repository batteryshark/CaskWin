param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
$output=$OutputDirectory
New-Item -ItemType Directory -Force -Path $output|Out-Null
try {
    foreach($a in $arches){
        $bits=if($a -eq 'x86'){'32'}else{'64'}
        $toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
        if(!(Test-Path -LiteralPath $compiler)){throw "Install the MSYS2 mingw$bits GCC toolchain to build registry fixtures."}
        $env:Path="$toolchain;$savedPath"
        $temporary=Join-Path $PSScriptRoot "build\registry-probe\$a"
        New-Item -ItemType Directory -Force -Path $temporary|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $target=& $compiler -dumpmachine
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if($LASTEXITCODE -or $target -ne $expected){throw "Unexpected registry fixture compiler target: $target"}
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++','-DUNICODE','-D_UNICODE')
        $source=Join-Path $PSScriptRoot 'registry-probe.cpp'
        & $compiler @flags -municode $source (Join-Path $PSScriptRoot 'registry-views.cpp') -o (Join-Path $output "registry-probe$bits.exe") -ladvapi32 -lole32 -luuid
        if($LASTEXITCODE){throw "Registry fixture build failed ($a)"}
        & $compiler @flags -shared -DWINEDOWS_REGISTRY_COM_DLL $source -o (Join-Path $output "registry-com$bits.dll") -lole32 -luuid '-Wl,--kill-at'
        if($LASTEXITCODE){throw "Registry COM server build failed ($a)"}
        Write-Host "Registry fixtures built: $a"
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
