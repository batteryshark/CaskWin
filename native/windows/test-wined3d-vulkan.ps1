param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'),[string[]]$NativeAdapterVendors=@())
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\wined3d-vulkan-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Force -Path $suite|Out-Null
$fixtures=if($SkipBuild){$RuntimeDirectory}else{Join-Path $suite 'fixtures'}
if(!$SkipBuild){& "$PSScriptRoot\build-wined3d-vulkan-probe.ps1" -OutputDirectory $fixtures;if($LASTEXITCODE){throw 'Vulkan lifecycle fixture build failed'}}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$prefix=Join-Path $suite 'prefix';$results=[Collections.Generic.List[object]]::new()
$savedOverrides=$env:WINEDLLOVERRIDES;$savedConfig=$env:WINE_D3D_CONFIG;$savedTrace=$env:WINEDOWS_TRACE
try{
 $env:WINEDLLOVERRIDES=$null;$env:WINE_D3D_CONFIG=$null;$env:WINEDOWS_TRACE=$null
 & $runtime --prefix $prefix init *> (Join-Path $suite 'init.log');if($LASTEXITCODE){throw 'Vulkan lifecycle prefix initialization failed'}
 $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Force -Path $tests|Out-Null
 foreach($bits in @(32,64)){Copy-Item -LiteralPath (Join-Path $fixtures "wined3d-vulkan-probe$bits.exe") -Destination $tests}
 foreach($bits in @(32,64)){foreach($provider in @('native','wine')){foreach($desktop in @('off','Vulkan,800x600')){foreach($empty in @($false,$true)){
  $label="$provider-$bits-$desktop-empty$empty";$log=Join-Path $suite "$label.log"
  if($provider -eq 'wine'){$env:WINEDLLOVERRIDES='dxgi,d3d11=b;vulkan-1=b';$env:WINE_D3D_CONFIG='renderer=vulkan'}else{$env:WINEDLLOVERRIDES='dxgi,d3d11=n;vulkan-1=n';$env:WINE_D3D_CONFIG=$null}
  $arguments=@('--prefix',$prefix,'--desktop',$desktop,'run',"C:\tests\wined3d-vulkan-probe$bits.exe","--$provider");if($empty){$arguments+='--empty-first'}
  & $runtime @arguments *> $log;$code=$LASTEXITCODE;$content=Get-Content -Raw -LiteralPath $log
  if($code -or $content -match '(?m)^FAIL ' -or $content -notmatch 'PASS complete device/swapchain lifecycle cycle=2' -or $content -notmatch 'PASS physical display mode unchanged'){throw "WineD3D Vulkan lifecycle failed: $label ($code)"}
  foreach($phase in @('resized-window-old-buffer','resized-window-second-present','resized-buffers','zero-height-window','empty-window','minimized','restored','recreated-swapchain')){if($content -notmatch ('WSI PHASE '+$phase+' ')){throw "Missing WSI phase $phase in $label"}}
  if($provider -eq 'wine'){
   $arch=if($bits -eq 32){'x86'}else{'x64'}
   foreach($record in @(@{prefix='D3D11 MODULE ';file='d3d11.dll'},@{prefix='WINE VULKAN MODULE ';file='winevulkan.dll'},@{prefix='WINED3D MODULE ';file='wined3d.dll'})){$expected=Join-Path $RuntimeDirectory "builtin\$arch\$($record.file)";if($content -notmatch ([Regex]::Escape($record.prefix+$expected))){throw "Selected builtin module mismatch: $($record.file) in $label"}}
   if($content -notmatch 'PASS real shader work and presentation traversed WineVulkan'){throw "Vulkan rendering was not proven: $label"}
  }
  if($content -notmatch 'VULKAN EVIDENCE submits=(\d+) presents=(\d+) recoverable_present_errors=(\d+)'){throw 'Vulkan evidence counters missing'}
  $results.Add(@{case=$label;status='passed';architecture=$bits;provider=$provider;desktop=$desktop;initiallyEmpty=$empty;deviceCycles=3;shaderPixels=432;queueSubmits=[int]$Matches[1];queuePresents=[int]$Matches[2];recoverablePresentErrors=[int]$Matches[3]});Write-Host "PASS $label"
 }}}}
 foreach($bits in @(32,64)){foreach($vendor in $NativeAdapterVendors){
  $label="native-explicit-$bits-$vendor";$log=Join-Path $suite "$label.log";$env:WINEDLLOVERRIDES='dxgi,d3d11=n;vulkan-1=n';$env:WINE_D3D_CONFIG=$null
  & $runtime --prefix $prefix --desktop off run "C:\tests\wined3d-vulkan-probe$bits.exe" --native --empty-first --adapter $vendor *> $log
  $code=$LASTEXITCODE;$content=Get-Content -Raw -LiteralPath $log
  if($code -or $content -match '(?m)^FAIL ' -or $content -notmatch 'PASS device uses explicitly selected hardware vendor' -or $content -notmatch 'PASS complete device/swapchain lifecycle cycle=2' -or $content -notmatch 'PASS physical display mode unchanged'){throw "Explicit native adapter lifecycle failed: $label ($code)"}
  $results.Add(@{case=$label;status='passed';architecture=$bits;provider='native';desktop='off';initiallyEmpty=$true;requestedVendor=$vendor;deviceCycles=3;shaderPixels=432});Write-Host "PASS $label"
 }}
 @{passed=$true;cases=$results;scope='Real D3D11 shaders and readback through WineD3D Vulkan; windowed presentation, empty/minimized windows, resizing, restoration, and swapchain recreation; physical display modes preserved'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 Write-Host "WineD3D Vulkan lifecycle suite passed: $suite"
}catch{@{passed=$false;cases=$results;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw}
finally{$env:WINEDLLOVERRIDES=$savedOverrides;$env:WINE_D3D_CONFIG=$savedConfig;$env:WINEDOWS_TRACE=$savedTrace}
$global:LASTEXITCODE=0
