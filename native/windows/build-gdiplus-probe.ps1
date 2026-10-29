param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-08\fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
foreach($bits in @(32,64)){
 $bin="C:\msys64\mingw$bits\bin";$env:Path="$bin;$env:Path";$env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
 foreach($static in @($false,$true)){
  $name=if($static){"gdiplus-import-probe$bits.exe"}else{"gdiplus-probe$bits.exe"}
  $arguments=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-municode',"$PSScriptRoot\gdiplus-probe.cpp",'-o',(Join-Path $OutputDirectory $name),'-lole32')
  if($static){$arguments+=@('-DWINEDOWS_STATIC_GDIPLUS','-lgdiplus')}
  & (Join-Path $bin 'g++.exe') @arguments
  if($LASTEXITCODE){throw "GDI+ fixture compile failed: $name"}
 }
}
$global:LASTEXITCODE=0
