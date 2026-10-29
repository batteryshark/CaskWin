param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-07\fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
foreach($bits in @(32,64)){
 $bin="C:\msys64\mingw$bits\bin";$env:Path="$bin;$env:Path";$env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
 $definition=Join-Path $OutputDirectory "d3dx11$bits.def"
 $symbols=if($bits -eq 32){@('D3DX11CheckVersion@8','D3DX11CompileFromMemory@52','D3DX11CreateTextureFromMemory@28')}else{@('D3DX11CheckVersion','D3DX11CompileFromMemory','D3DX11CreateTextureFromMemory')}
 $nl=[Environment]::NewLine
 [IO.File]::WriteAllText($definition,("LIBRARY d3dx11_43.dll"+$nl+"EXPORTS"+$nl+($symbols -join $nl)+$nl),[Text.UTF8Encoding]::new($false))
 $library=Join-Path $OutputDirectory "d3dx11$bits.a"
 & (Join-Path $bin 'dlltool.exe') --kill-at --input-def $definition --output-lib $library
 if($LASTEXITCODE){throw "D3DX11 import library failed $bits"}
 foreach($static in @($false,$true)){
  $name=if($static){"d3dx11-import-probe$bits.exe"}else{"d3dx11-probe$bits.exe"}
  $arguments=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-municode',"$PSScriptRoot\d3dx11-probe.cpp",'-o',(Join-Path $OutputDirectory $name),'-ldxguid','-lole32','-luuid')
  if($static){$arguments+=@('-DWINEDOWS_STATIC_D3DX11',$library)}
  & (Join-Path $bin 'g++.exe') @arguments
  if($LASTEXITCODE){throw "D3DX11 fixture compile failed: $name"}
 }
}
$global:LASTEXITCODE=0
