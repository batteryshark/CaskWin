param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'))
$ErrorActionPreference='Stop'
$savedPath=$env:Path
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
try{
 foreach($a in $arches){
  $bits=if($a -eq 'x86'){32}else{64}
  $toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
  $env:Path="$toolchain;$savedPath"
  $target=& $compiler -dumpmachine
  $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
  if($LASTEXITCODE -or $target -ne $expected){throw "Unexpected fixture compiler target: $target"}
  & $compiler -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -municode (Join-Path $PSScriptRoot 'registry-transfer-probe.cpp') -o (Join-Path $OutputDirectory "registry-transfer-probe$bits.exe") -ladvapi32
  if($LASTEXITCODE){throw "Registry transfer fixture build failed ($a)"}
  Write-Host "Registry transfer fixture built: $a"
 }
}finally{$env:Path=$savedPath}
$global:LASTEXITCODE=0
