param([string]$Name='caskwin-0.11-alpha',[string]$RuntimeDirectory,[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\wine-source.ps1"
. "$PSScriptRoot\package-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$faudioLicense=Join-Path $wineSource 'libs\faudio\LICENSE'
if(!(Test-Path -LiteralPath $faudioLicense -PathType Leaf)){throw "Missing selected Wine source license: $faudioLicense"}
if($Name -notmatch '^[a-zA-Z0-9][a-zA-Z0-9._-]*$'){throw 'Package name must be a single plain directory name.'}
$repo=Split-Path (Split-Path $PSScriptRoot)
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$buildSourceManifest=Assert-CaskWinPackageSource -RuntimeDirectory $RuntimeDirectory -WineSourceDirectory $wineSource
$dest=Join-Path $PSScriptRoot "dist\$Name"
if(Test-Path -LiteralPath $dest){throw 'Package directory already exists; choose a new name.'}
$required=@('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')
foreach($file in $required){if(!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory $file))){throw "Missing $file; run build.ps1 first."}}
$builtins=@('ddraw','d3d8','d3d9','wined3d','winevulkan','vulkan-1','dxgi','d3d11','d3d10core','d3d10_1','d3d10','d3dcompiler_39','dinput','dinput8','xinput1_1','xinput1_2','xinput1_3','xinput1_4','xinput9_1_0','dsound','mcicda','d3dx9_36','d3dx9_37','d3dx9_41','d3dx9_42','d3dcompiler_42','d3dx9_43','d3dx10_43','d3dx11_43','d3dcompiler_43','d3dcompiler_47','d3dxof','xaudio2_7','xaudio2_8','xaudio2_9','x3daudio1_7','xapofx1_5','mfreadwrite','gdiplus','xactengine3_7','glu32')
foreach($arch in @('x86','x64')){foreach($module in $builtins){if(!(Test-Path -LiteralPath "$RuntimeDirectory\builtin\$arch\$module.dll")){throw "Missing $arch/$module.dll; run build-wine.ps1 for both architectures first."}}}
New-Item -ItemType Directory -Path $dest | Out-Null
foreach($file in $required){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $file) -Destination $dest}
Copy-Item -LiteralPath $buildSourceManifest -Destination $dest
Copy-Item -LiteralPath (Join-Path $dest 'wine64.exe') -Destination (Join-Path $dest 'wine.exe')
foreach($arch in @('x86','x64')){
    $target=Join-Path $dest "builtin\$arch"
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach($module in $builtins){Copy-Item -LiteralPath "$RuntimeDirectory\builtin\$arch\$module.dll" -Destination $target}
}
Copy-Item -LiteralPath "$repo\COPYING.LIB" -Destination (Join-Path $dest 'LICENSE-Wine.txt')
Copy-Item -LiteralPath "$PSScriptRoot\vendor\Detours-4.0.1\LICENSE.md" -Destination (Join-Path $dest 'LICENSE-Detours.txt')
Copy-Item -LiteralPath $faudioLicense -Destination (Join-Path $dest 'LICENSE-FAudio.txt')
Copy-Item -LiteralPath "$PSScriptRoot\glu32-notices.txt" -Destination (Join-Path $dest 'LICENSE-GLU.txt')
Copy-Item -LiteralPath "$repo\README.caskwin.md" -Destination $dest
Copy-Item -LiteralPath "$PSScriptRoot\prepare-cdda.ps1" -Destination $dest
@'
CaskWin 0.11 alpha - native Windows backend with Wine graphics, input, and audio

For compatibility, CaskWin keeps the wine.exe, wine32.exe, wine64.exe,
winedows32.dll and winedows64.dll filenames. Existing .winedows prefix metadata
and WINEDOWS_* settings retain their names and behavior.

PowerShell quick start:
  .\wine.exe --prefix .\prefixes\game init
  $env:WINEDLLOVERRIDES='ddraw,d3d8,d3d9,dinput,dinput8,dsound,mcicda=b,n'
  .\wine.exe --prefix .\prefixes\game run 'C:\Games\Example\game.exe'
  .\wine.exe --prefix .\prefixes\game --desktop Game,800x600 run 'C:\Games\Example\game.exe'
  .\wine.exe --prefix .\prefixes\game drive mount D 'D:\Images\Game.iso'
  .\wine.exe --prefix .\prefixes\game drive mount D 'D:\Images\MixedModeGame.cue'
  # In another terminal while that prefix is running:
  .\wine.exe --prefix .\prefixes\game drive swap D 'D:\Images\Disc2.iso'
  .\wine.exe --prefix .\prefixes\game drive eject D
  .\wine.exe --prefix .\prefixes\game drive load D

Place/install the game in that prefix's drive_c first.
Use n,b or n to select native Windows/application DLLs.
No overrides currently means native. Select d3d11=b,n for the Wine DXGI/D3D10/11
family, or xinput1_4=b,n for Wine XInput with the native Windows device backend.
Other helpers include d3dx9_36/37/41/42/43, d3dx10_43, d3dx11_43, d3dcompiler_39/42/43/47, d3dxof, xaudio2_7/8/9,
x3daudio1_7, xapofx1_5, mfreadwrite, gdiplus, and xactengine3_7. Dependencies are resolved automatically. See the
README.caskwin.md for tested behavior, controller coverage, and effect limitations.
D3DX10 shader/texture helpers, matrix math, state effects and precompiled shader
effects are tested. Sprite/font drawing and FX source with embedded CompileShader
remain incomplete; select native helpers when a game needs those features.
Wine glu32 provides projection, tessellation, quadrics and texture mipmap helpers
on the native OpenGL driver. Use glu32=b to select it; keep opengl32=n. Wine GLU's
NURBS implementation is incomplete; use glu32=n for applications needing NURBS.
Fallback selects an available module and does not retry individual API failures.
An empty override such as example= disables that DLL for executable loads,
including startup dependencies and children. Resource-only loads remain available.
Core/KnownDLL/API-set disabling and conflicting exact/wildcard orders are rejected.
Legacy WinExec and unquoted child paths use the prefix-aware process backend.
Wine's per-application win98 version also preserves full ANSI registry strings
for older programs that supply incorrect REG_SZ byte counts.
Wine Vulkan is opt-in, with Vulkan 1.0 and selected extensions:
  $env:WINEDLLOVERRIDES='d3d11=b,n;vulkan-1=b,n'
  $env:WINE_D3D_CONFIG='renderer=vulkan'
  .\wine.exe --prefix .\prefixes\game run 'C:\Games\Example\game.exe'
  Remove-Item Env:WINE_D3D_CONFIG
Use vulkan-1=n for native Vulkan. Availability fallback does not retry unsupported
Vulkan features. WineD3D currently uses its first Vulkan GPU.
Named application synchronization objects are shared inside each prefix session.
The virtual desktop preserves application DPI awareness and reports private monitor
bounds. A resizable game window can be maximized inside it by double-clicking its
inner title bar. Set WINEDOWS_TRACE_DESKTOP=1 for bounded geometry logs in the
prefix's .winedows folder when diagnosing a sizing problem.
Existing menu-bearing windows created hidden retain their native menus when
shown in the desktop. Visible-at-creation menus and menus added after child
conversion remain limited. Full Tilt's dark menu-bar appearance is still observed.
Live ISO/CUE swaps preserve the configured startup disc; old data handles must
be reopened. Media-change notifications reach the game's process tree, with
bounded delivery for hung windows. Direct NT CD controls require synchronous handles.
Inherited and duplicated registry handles preserve their native 32/64-bit view.
For prepared CD audio tracks, see prepare-cdda.ps1 and README.caskwin.md. FFmpeg is needed
only during track preparation; games consume private PCM through Wine mcicda.
Full Wine compatibility parity is not implemented. This is not a security sandbox.
See README.caskwin.md for test coverage and remaining limits.

CaskWin 0.10 source:
https://github.com/Batteryshark/CaskWin/tree/v0.10-alpha
The included caskwin-build-source.json records this runtime's actual Wine base
commit, patched tree, profile and ordered patches. Packaging verifies that record
against the selected prepared source and includes it in SHA256SUMS.txt.
Default validated Wine profile: Wine 11.0, db11d0fe6a169c457e23d007e20404643d067aa8
https://github.com/wine-mirror/wine/tree/wine-11.0
This is a standalone backend repository. Native sources and build scripts are in
native/windows; vendor/wine is a pristine upstream submodule. wine-source.json
selects an ordered patch profile from patches/wine-11.0. scripts/prepare-wine.ps1 makes
the patched build/wine-source copy without editing the submodule.
Inside that prepared Wine tree, portability patches affect
tools/winebuild/import.c, tools/wrc/utils.c, dlls/dsound/primary.c,
dlls/mcicda/mcicda.c, dlls/xaudio2_7/xapo.c, dlls/dxgi/dxgi_main.c,
dlls/d3d11/d3d11_main.c, dlls/wined3d/context_gl.c,
dlls/xinput1_3/main.c, dlls/xinput1_4/xinput1_4.spec, dlls/mfreadwrite/reader.c,
libs/vkd3d, dlls/dxgi/factory.c, dlls/wined3d/swapchain.c, dlls/winevulkan,
include/wine/vulkan_driver.h, dlls/d3dx11_42, dlls/d3dx11_43 and include/d3dx11core.idl.
The D3DX10 version contract corrections are in dlls/d3dx10_33 through d3dx10_43
and include/d3dx10core.h.
Additional native font and wave-lifetime adaptations are in dlls/gdiplus/font.c,
dlls/xactengine3_7/xact_dll.c, and libs/faudio/src/FACT*.
The glu32 helper uses unchanged Wine sources in dlls/glu32. See LICENSE-GLU.txt
for its SGI and other notices.
Preserve these sources when redistributing changes.
When packaging a different Wine build, use the same -WineSourceDirectory as the
build and preserve its source revision and patch profile. LICENSE-FAudio.txt is
copied from that selected source tree; other Wine versions require validation.
Wine code is LGPL-2.1-or-later. Detours is MIT; FAudio is zlib-licensed.
Runtime DLLs remain replaceable.
The vendored Detours creatwth.cpp includes a process-bitness check for AnyCPU
launches from 32-bit callers; its original MIT license is preserved.
'@ | Set-Content -LiteralPath (Join-Path $dest 'START-HERE.txt')
Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object {$_.Name -ne 'SHA256SUMS.txt'} | ForEach-Object {
    $relative=$_.FullName.Substring($dest.Length+1)
    "$( (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash )  $relative"
} | Set-Content -LiteralPath (Join-Path $dest 'SHA256SUMS.txt')
$archive=$dest+'.zip'
if(Test-Path -LiteralPath $archive){throw 'Package archive already exists.'}
Compress-Archive -LiteralPath $dest -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Packaged: $archive"
