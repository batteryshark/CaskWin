param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-07'))
$ErrorActionPreference='Stop'
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
try{foreach($a in $arches){
 $bits=if($a -eq 'x86'){32}else{64};$toolchain="C:\msys64\mingw$bits\bin";$env:Path="$toolchain;$savedPath"
 $build=Join-Path $PSScriptRoot "build\override-disabled\$a";New-Item -ItemType Directory -Force -Path $build,$OutputDirectory|Out-Null
 $env:TEMP=$build;$env:TMP=$build
 $compiler=Join-Path $toolchain 'g++.exe';$target=& $compiler -dumpmachine;$expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
 if($LASTEXITCODE -or $target -ne $expected){throw "Wrong compiler target: $target"}
 & (Join-Path $toolchain 'windres.exe') (Join-Path $PSScriptRoot 'override-disabled-probe.rc') (Join-Path $build 'marker-res.o')
 if($LASTEXITCODE){throw 'Override fixture resources failed'}
 $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++','-DUNICODE','-D_UNICODE')
 $source=Join-Path $PSScriptRoot 'override-disabled-probe.cpp';$library=Join-Path $build 'marker.a'
 & $compiler @flags -shared -DOVERRIDE_MARKER_DLL $source (Join-Path $build 'marker-res.o') -o (Join-Path $build 'override-marker.dll') '-Wl,--kill-at' "-Wl,--out-implib,$library"
 if($LASTEXITCODE){throw 'Override marker DLL build failed'}
 Copy-Item -LiteralPath (Join-Path $build 'override-marker.dll') -Destination (Join-Path $OutputDirectory "override-marker$bits.dll")
 & $compiler @flags -shared -DOVERRIDE_BRIDGE_DLL $source $library -o (Join-Path $OutputDirectory "override-bridge$bits.dll") '-Wl,--kill-at'
 if($LASTEXITCODE){throw 'Override bridge DLL build failed'}
 & $compiler @flags -municode $source -o (Join-Path $OutputDirectory "override-disabled-probe$bits.exe")
 if($LASTEXITCODE){throw 'Override executable build failed'}
 & $compiler @flags -municode -DOVERRIDE_STATIC_IMPORT $source $library -o (Join-Path $OutputDirectory "override-disabled-static$bits.exe")
 if($LASTEXITCODE){throw 'Override static executable build failed'}
 Write-Host "Disabled override fixtures built: $a"
}}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
