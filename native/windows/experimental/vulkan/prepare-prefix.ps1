param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\..\bin'),[string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$prefix=Join-Path $StageDirectory 'prefix'
& (Join-Path $RuntimeDirectory 'wine64.exe') --prefix $prefix init
if($LASTEXITCODE){throw 'Vulkan isolated prefix initialization failed'}
foreach($bits in @(32,64)){
 $arch=if($bits -eq 32){'x86'}else{'x64'};$dest=Join-Path $prefix "drive_c\vulkan$bits"
 New-Item -ItemType Directory -Force -Path $dest|Out-Null
 Copy-Item -LiteralPath (Join-Path $StageDirectory "probe$bits.exe") -Destination $dest
 foreach($name in @('winevulkan.dll','vulkan-1.dll')){Copy-Item -LiteralPath (Join-Path $StageDirectory "$arch\$name") -Destination $dest}
 Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "d3d11-probe$bits.exe") -Destination $dest
}
