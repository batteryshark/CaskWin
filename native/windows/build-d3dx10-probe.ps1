param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-09\fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
 foreach($bits in @(32,64)){
  $bin="C:\msys64\mingw$bits\bin";$env:Path="$bin;$savedPath";$env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
  $expected=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
  if((& (Join-Path $bin 'g++.exe') -dumpmachine) -ne $expected){throw 'Wrong MinGW architecture'}
  $definition=Join-Path $OutputDirectory "d3dx10$bits.def"
  $exports=@('D3DX10CheckVersion@8','D3DX10CompileFromMemory@52','D3DX10CreateTextureFromMemory@28','D3DX10CreateEffectFromMemory@56','D3DXMatrixMultiply@12','D3DXMatrixTranslation@16','D3DXMatrixScaling@16')
  if($bits -eq 64){$exports=@($exports|ForEach-Object {$_ -replace '@[0-9]+$',''})}
  $nl=[Environment]::NewLine
  [IO.File]::WriteAllText($definition,("LIBRARY d3dx10_43.dll"+$nl+"EXPORTS"+$nl+($exports -join $nl)+$nl),[Text.UTF8Encoding]::new($false))
  $library=Join-Path $OutputDirectory "d3dx10$bits.a"
  & (Join-Path $bin 'dlltool.exe') --kill-at --input-def $definition --output-lib $library
  if($LASTEXITCODE){throw "D3DX10 import library failed $bits"}
  foreach($static in @($false,$true)){
   $name=if($static){"d3dx10-import-probe$bits.exe"}else{"d3dx10-probe$bits.exe"}
   $output=Join-Path $OutputDirectory $name
   $arguments=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-municode',"$PSScriptRoot\d3dx10-probe.cpp",'-o',$output,'-ldxguid','-lole32','-luuid')
   if($static){$arguments+=@('-DWINEDOWS_STATIC_D3DX10',$library)}
   & (Join-Path $bin 'g++.exe') @arguments
   if($LASTEXITCODE){throw "D3DX10 fixture compile failed: $name"}
   $imports=& (Join-Path $bin 'objdump.exe') -p $output|Select-String 'DLL Name:'
   if($LASTEXITCODE -or $imports -match 'libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin'){throw "Unexpected fixture runtime imports: $name"}
   Write-Host "Built $name ($expected; static C/C++ runtimes)"
  }
 }
 # Generate a GPU-independent application asset using the fixed native compiler;
 # Wine providers subsequently parse/apply/render these same bytes in the tests.
 & (Join-Path $OutputDirectory 'd3dx10-probe64.exe') --generate-effect (Join-Path $OutputDirectory 'd3dx10-draw.fxo')
 if($LASTEXITCODE){throw 'Independent FX10 fixture generation failed'}
} finally {$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
