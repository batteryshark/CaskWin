param([string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop';$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
. (Join-Path $PSScriptRoot '..\..\wine-source.ps1')
$repo=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
foreach($bits in @(32,64)){
 $bin="C:\msys64\mingw$bits\bin";$out=Join-Path $StageDirectory "probe$bits.exe";$env:Path="$bin;$env:Path";$env:TEMP=$StageDirectory;$env:TMP=$StageDirectory
 & (Join-Path $bin 'g++.exe') -std=c++17 -O2 -g -static -static-libgcc -static-libstdc++ -municode ("-I"+(Join-Path $StageDirectory 'generated')) ("-I"+(Join-Path $repo 'libs\vkd3d\include')) (Join-Path $PSScriptRoot 'probe.cpp') -o $out -luser32
 if($LASTEXITCODE){throw "Probe build failed $bits"}
 $arch=if($bits -eq 32){'x86'}else{'x64'};Copy-Item -LiteralPath $out -Destination (Join-Path $StageDirectory $arch)
 $definition=Join-Path $StageDirectory "vulkan-import$bits.def"
 $symbol=if($bits -eq 32){'vkGetInstanceProcAddr@8'}else{'vkGetInstanceProcAddr'}
 [IO.File]::WriteAllText($definition,"LIBRARY vulkan-1.dll`nEXPORTS`n$symbol`n",[Text.UTF8Encoding]::new($false))
 $library=Join-Path $StageDirectory "vulkan-import$bits.a"
 & (Join-Path $bin 'dlltool.exe') --kill-at --input-def $definition --output-lib $library
 if($LASTEXITCODE){throw "Vulkan import library failed $bits"}
 $staticOut=Join-Path $StageDirectory "import-probe$bits.exe"
 & (Join-Path $bin 'g++.exe') -std=c++17 -O2 -g -static -static-libgcc -static-libstdc++ -municode -DWINEDOWS_VULKAN_STATIC_PROBE ("-I"+(Join-Path $StageDirectory 'generated')) ("-I"+(Join-Path $repo 'libs\vkd3d\include')) (Join-Path $PSScriptRoot 'probe.cpp') $library -o $staticOut -luser32
 if($LASTEXITCODE){throw "Static probe build failed $bits"}
 Copy-Item -LiteralPath $staticOut -Destination (Join-Path $StageDirectory $arch)
}
