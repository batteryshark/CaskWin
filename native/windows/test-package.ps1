param([Parameter(Mandatory=$true)][string]$Archive,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\wine-source.ps1"
. "$PSScriptRoot\package-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$gdiplusFont=Join-Path $wineSource 'dlls\gdi32\tests\wine_test.ttf'
$gdiplusFontHash=(Get-FileHash -LiteralPath $gdiplusFont -Algorithm SHA256).Hash
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\package-'+[Guid]::NewGuid().ToString('N'))
$extracted=Join-Path $suite 'extracted'
New-Item -ItemType Directory -Path $extracted -Force | Out-Null
Expand-Archive -LiteralPath $Archive -DestinationPath $extracted
$folders=@(Get-ChildItem -LiteralPath $extracted -Directory)
if($folders.Count -ne 1){throw 'Expected one runtime directory in package'}
$runtime=$folders[0].FullName
$manifest=Join-Path $runtime 'SHA256SUMS.txt'
$verified=0
$listed=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($line in Get-Content -LiteralPath $manifest){
    if($line -notmatch '^([A-Fa-f0-9]{64})  (.+)$'){throw 'Invalid package hash manifest'}
    $expected=$Matches[1];$relative=$Matches[2]
    $file=[IO.Path]::GetFullPath((Join-Path $runtime $relative))
    if(!$file.StartsWith($runtime+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Hash manifest path escapes runtime'}
    if(!$listed.Add($relative)){throw "Duplicate package hash entry: $relative"}
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected){throw "Package hash mismatch: $relative"}
    $verified++
}
$files=@(Get-ChildItem -LiteralPath $runtime -Recurse -File | Where-Object {$_.FullName -ne $manifest})
if($files.Count -ne $verified){throw 'Package contains files missing from hash manifest'}
if(!$listed.Contains('caskwin-build-source.json')){throw 'Package hash manifest omits caskwin-build-source.json'}
Assert-CaskWinPackageSource -RuntimeDirectory $runtime -WineSourceDirectory $wineSource|Out-Null
$modules=@('ddraw','d3d8','d3d9','wined3d','winevulkan','vulkan-1','dxgi','d3d11','d3d10core','d3d10_1','d3d10','d3dcompiler_39','dinput','dinput8','xinput1_1','xinput1_2','xinput1_3','xinput1_4','xinput9_1_0','dsound','mcicda','d3dx9_36','d3dx9_37','d3dx9_41','d3dx9_42','d3dcompiler_42','d3dx9_43','d3dx10_43','d3dx11_43','d3dcompiler_43','d3dcompiler_47','d3dxof','xaudio2_7','xaudio2_8','xaudio2_9','x3daudio1_7','xapofx1_5','mfreadwrite','gdiplus','xactengine3_7','glu32')
foreach($arch in @('x86','x64')){foreach($module in $modules){
    if(!(Test-Path -LiteralPath "$runtime\builtin\$arch\$module.dll")){throw "Missing packaged $arch/$module"}
}}
$prefix=Join-Path $suite 'prefix'
$loader=Join-Path $runtime 'wine64.exe'
& $loader --prefix $prefix init
if($LASTEXITCODE){throw 'Packaged prefix initialization failed'}
$renderer=Join-Path $suite 'renderer.reg'
@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\Direct3D]
"renderer"="gl"
'@ | Set-Content -LiteralPath $renderer
& $loader --prefix $prefix reg import $renderer
if($LASTEXITCODE){throw 'Packaged renderer setup failed'}
$programs=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $programs | Out-Null
Copy-Item -LiteralPath $gdiplusFont -Destination (Join-Path $programs 'wine_test.ttf')
Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'd3dx10-draw.fxo') -Destination $programs
& (Join-Path $FixtureDirectory 'video-probe64.exe') --generate (Join-Path $programs 'generated.mp4') *> (Join-Path $suite 'generate-video.log')
if($LASTEXITCODE){throw 'Cannot generate package video fixture'}
& (Join-Path $FixtureDirectory 'av-probe64.exe') --generate (Join-Path $programs 'generated-av.mp4') *> (Join-Path $suite 'generate-av.log')
if($LASTEXITCODE){throw 'Cannot generate package audiovisual fixture'}
$savedOverrides=$env:WINEDLLOVERRIDES
$savedD3DConfig=$env:WINE_D3D_CONFIG
$savedMarker=$env:WINEDOWS_OVERRIDE_MARKER
$checks=[Collections.Generic.List[object]]::new()
try {
    $env:WINE_D3D_CONFIG=$null
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $builtin=Join-Path $runtime "builtin\$arch"
        foreach($fixture in @("d3dx9-import-probe$bits.exe","d3dx11-import-probe$bits.exe","xaudio9-import-probe$bits.exe","d3d11-import-probe$bits.exe","xinput-import-1_4-$bits.exe","compiler47-import-probe$bits.exe","video-probe$bits.exe")){
            Copy-Item -LiteralPath (Join-Path $FixtureDirectory $fixture) -Destination $programs
        }
        $env:WINEDLLOVERRIDES='d3dx9_43=b;d3d9=n'
        & $loader --prefix $prefix --desktop 'Package,800x600' run "C:\tests\d3dx9-import-probe$bits.exe" --expected-dll "$builtin\d3dx9_43.dll" *> (Join-Path $suite "d3dx-$bits.log")
        if($LASTEXITCODE){throw "Packaged D3DX render/import smoke failed ($bits-bit)"}
        $checks.Add(@{fixture="d3dx-$bits";passed=$true})
        foreach($version in @(37,41,42)){
            $fixture="d3dx9-$version-import-probe$bits.exe"
            Copy-Item -LiteralPath (Join-Path $FixtureDirectory $fixture) -Destination $programs
            $env:WINEDLLOVERRIDES="d3dx9_$version=b;d3d9=n"
            & $loader --prefix $prefix run "C:\tests\$fixture" --expected-dll "$builtin\d3dx9_$version.dll" *> (Join-Path $suite "d3dx$version-$bits.log")
            if($LASTEXITCODE){throw "Packaged D3DX$version render/import smoke failed ($bits-bit)"}
            $checks.Add(@{fixture="d3dx$version-$bits";passed=$true})
        }
        $env:WINEDLLOVERRIDES='xaudio2_9,x3daudio1_7,xapofx1_5=b'
        & $loader --prefix $prefix run "C:\tests\xaudio9-import-probe$bits.exe" xaudio2_9.dll "$builtin\xaudio2_9.dll" $builtin *> (Join-Path $suite "xaudio-$bits.log")
        if($LASTEXITCODE){throw "Packaged XAudio stream/import smoke failed ($bits-bit)"}
        $checks.Add(@{fixture="xaudio-$bits";passed=$true})
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "xact-probe$bits.exe") -Destination $programs
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'test.xwb') -Destination (Join-Path $programs 'xact-test.xwb')
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'silent-streaming.xwb') -Destination (Join-Path $programs 'xact-streaming.xwb')
        $env:WINEDLLOVERRIDES='xactengine3_7=b'
        $xactLog=Join-Path $suite "xact-$bits.log"
        & $loader --prefix $prefix run "C:\tests\xact-probe$bits.exe" "$builtin\xactengine3_7.dll" 'C:\tests\xact-test.xwb' --com *> $xactLog
        if($LASTEXITCODE -or (Get-Content -Raw -LiteralPath $xactLog) -notmatch '(?m)^PASS XACT engine playback, timing and notifications'){
            throw "Packaged XACT engine/COM playback smoke failed ($bits-bit)"
        }
        $checks.Add(@{fixture="xact-$bits";passed=$true})
        $xactLog=Join-Path $suite "xact-managed-$bits.log"
        & $loader --prefix $prefix run "C:\tests\xact-probe$bits.exe" "$builtin\xactengine3_7.dll" 'C:\tests\xact-streaming.xwb' --com-ex --streaming --no-handle-only *> $xactLog
        if($LASTEXITCODE -or (Get-Content -Raw -LiteralPath $xactLog) -notmatch '(?m)^PASS XACT no-handle playback, cancellation and lifetime'){
            throw "Packaged XACT managed-wave streaming smoke failed ($bits-bit)"
        }
        $checks.Add(@{fixture="xact-managed-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='d3d11=b'
        & $loader --prefix $prefix --desktop 'Package,800x600' run "C:\tests\d3d11-import-probe$bits.exe" --wine *> (Join-Path $suite "d3d11-$bits.log")
        if($LASTEXITCODE){throw "Packaged D3D11 shader/render/import smoke failed ($bits-bit)"}
        $checks.Add(@{fixture="d3d11-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='xinput1_4=b'
        & $loader --prefix $prefix run "C:\tests\xinput-import-1_4-$bits.exe" xinput1_4.dll *> (Join-Path $suite "xinput-$bits.log")
        if($LASTEXITCODE){throw "Packaged XInput query/import smoke failed ($bits-bit)"}
        $checks.Add(@{fixture="xinput-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='d3dcompiler_47=b;d3d11=n'
        $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
        & $loader --prefix $prefix run "C:\tests\compiler47-import-probe$bits.exe" "$builtin\d3dcompiler_47.dll" "$system\d3d11.dll" *> (Join-Path $suite "compiler47-$bits.log")
        if($LASTEXITCODE){throw "Packaged compiler47 render/compute/import smoke failed ($bits-bit)"}
        $checks.Add(@{fixture="compiler47-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='d3dx11_43=b;d3dcompiler_47=b;dxgi,d3d11=n;d3dcompiler_43=n'
        $helperLog=Join-Path $suite "d3dx11-$bits.log"
        & $loader --prefix $prefix run "C:\tests\d3dx11-import-probe$bits.exe" "$builtin\d3dx11_43.dll" "$system\d3d11.dll" "$builtin\d3dcompiler_47.dll" wine *> $helperLog
        if($LASTEXITCODE -or (Get-Content -Raw -LiteralPath $helperLog) -notmatch 'PASS D3DX11 helper version'){
            throw "Packaged D3DX11 shader/texture/import smoke failed ($bits-bit)"
        }
        $checks.Add(@{fixture="d3dx11-$bits";passed=$true})
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "d3dx10-import-probe$bits.exe") -Destination $programs
        $env:WINEDLLOVERRIDES='d3dx10_43=b;d3d10,d3d10_1,d3d10core,dxgi,d3d11=b;d3dcompiler_43=n'
        $helperLog=Join-Path $suite "d3dx10-$bits.log"
        & $loader --prefix $prefix run "C:\tests\d3dx10-import-probe$bits.exe" "$builtin\d3dx10_43.dll" "$builtin\d3d10_1.dll" "$builtin\d3dcompiler_47.dll" "$builtin\d3dx9_36.dll" wine 'C:\tests\d3dx10-draw.fxo' *> $helperLog
        if($LASTEXITCODE){throw "Packaged D3DX10 shader/effect/texture/math smoke failed ($bits-bit)"}
        $helperText=Get-Content -Raw -LiteralPath $helperLog
        if($helperText -notmatch 'PASS D3DX10 helper version / math / SM4 compile / effect / textures / 64 GPU pixels' -or
           $helperText -notmatch [Regex]::Escape("Helper=$builtin\d3dx10_43.dll") -or
           $helperText -notmatch [Regex]::Escape("Math=$builtin\d3dx9_36.dll")){
            throw "Packaged D3DX10 helper or forwarded Wine math provider was not exercised ($bits-bit)"
        }
        $checks.Add(@{fixture="d3dx10-$bits";passed=$true})
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "glu32-import-default$bits.exe") -Destination $programs
        $env:WINEDLLOVERRIDES='glu32=b;opengl32=n'
        $gluLog=Join-Path $suite "glu32-$bits.log"
        & $loader --prefix $prefix run "C:\tests\glu32-import-default$bits.exe" "$builtin\glu32.dll" wine *> $gluLog
        if($LASTEXITCODE){throw "Packaged GLU math/tessellation/texture/import smoke failed ($bits-bit)"}
        $gluText=Get-Content -Raw -LiteralPath $gluLog
        if($gluText -notmatch '(?m)^SUMMARY failures=0 .*cpuOnly=0' -or
           $gluText -notmatch '(?m)^OPENGL_IMPORTS count=\d+ native=1' -or
           $gluText -notmatch '(?m)^MIPMAP level=0 '){
            throw "Packaged Wine GLU or native OpenGL path was not exercised ($bits-bit)"
        }
        $checks.Add(@{fixture="glu32-$bits";passed=$true})
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "gdiplus-import-probe$bits.exe") -Destination $programs
        $env:WINEDLLOVERRIDES='gdiplus=b'
        $gdiplusLog=Join-Path $suite "gdiplus-$bits.log"
        & $loader --prefix $prefix run "C:\tests\gdiplus-import-probe$bits.exe" "$builtin\gdiplus.dll" 'C:\tests\wine_test.ttf' "C:\tests\gdiplus-$bits.png" *> $gdiplusLog
        if($LASTEXITCODE){throw "Packaged GDI+ static-import/font/PNG smoke failed ($bits-bit)"}
        $gdiplusText=Get-Content -Raw -LiteralPath $gdiplusLog
        if($gdiplusText -notmatch [Regex]::Escape("Provider=$builtin\gdiplus.dll") -or
           $gdiplusText -notmatch '(?m)^PASS GDI\+ helper provider / paths / codecs / private fonts' -or
           $gdiplusText -notmatch '(?m)^Private font=wine_test' -or
           !(Test-Path -LiteralPath (Join-Path $programs "gdiplus-$bits.png"))){
            throw "Packaged Wine GDI+ provider or private-font/PNG output was not exercised ($bits-bit)"
        }
        if((Get-FileHash -LiteralPath $gdiplusFont -Algorithm SHA256).Hash -ne $gdiplusFontHash -or
           (Get-FileHash -LiteralPath (Join-Path $programs 'wine_test.ttf') -Algorithm SHA256).Hash -ne $gdiplusFontHash){
            throw "Packaged GDI+ smoke changed its source or private font ($bits-bit)"
        }
        $checks.Add(@{fixture="gdiplus-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='mfreadwrite=b;dxgi,d3d11=n'
        $videoLog=Join-Path $suite "video-$bits.log"
        & $loader --prefix $prefix --desktop 'Package,800x600' run "C:\tests\video-probe$bits.exe" --async-lifecycle-wine 'C:\tests\generated.mp4' *> $videoLog
        if($LASTEXITCODE){throw "Packaged Wine video reader lifecycle smoke failed ($bits-bit)"}
        $videoText=Get-Content -Raw -LiteralPath $videoLog
        if($videoText -notmatch [Regex]::Escape("SOURCE READER OBJECT $builtin\mfreadwrite.dll") -or $videoText -notmatch 'successful native DXVA decode submissions prove hardware use'){
            throw "Packaged Wine reader or hardware decoder was not exercised ($bits-bit)"
        }
        $checks.Add(@{fixture="video-$bits";passed=$true})
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "av-probe$bits.exe") -Destination $programs
        $avLog=Join-Path $suite "av-$bits.log"
        & $loader --prefix $prefix run "C:\tests\av-probe$bits.exe" --async-queued 'C:\tests\generated-av.mp4' *> $avLog
        if($LASTEXITCODE){throw "Packaged Wine audiovisual queued-read smoke failed ($bits-bit)"}
        $avText=Get-Content -Raw -LiteralPath $avLog
        if($avText -notmatch [Regex]::Escape("SOURCE READER OBJECT $builtin\mfreadwrite.dll") -or $avText -notmatch 'real native DXVA submissions accompany converted video' -or $avText -notmatch 'AAC decoded channel preserves generated tone and level'){
            throw "Packaged audiovisual providers or decoded outputs were not exercised ($bits-bit)"
        }
        $checks.Add(@{fixture="av-$bits";passed=$true})
        $disabledApp=Join-Path $programs "disabled$bits"
        New-Item -ItemType Directory -Path $disabledApp | Out-Null
        foreach($fixture in @("override-disabled-probe$bits.exe","override-disabled-static$bits.exe")){
            Copy-Item -LiteralPath (Join-Path $FixtureDirectory $fixture) -Destination $disabledApp
        }
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "override-marker$bits.dll") -Destination (Join-Path $disabledApp 'override-marker.dll')
        $marker=Join-Path $disabledApp 'loaded.marker'
        $env:WINEDOWS_OVERRIDE_MARKER="C:\tests\disabled$bits\loaded.marker"
        $env:WINEDLLOVERRIDES='override-marker=n'
        & $loader --prefix $prefix run "C:\tests\disabled$bits\override-disabled-probe$bits.exe" --native override-marker.dll *> (Join-Path $suite "disabled-control-$bits.log")
        if($LASTEXITCODE -or !(Test-Path -LiteralPath $marker)){throw "Packaged disabled-DLL native control failed ($bits-bit)"}
        Remove-Item -LiteralPath $marker
        $checks.Add(@{fixture="disabled-control-$bits";passed=$true})
        $env:WINEDLLOVERRIDES='override-marker='
        foreach($mode in @('blocked','resource')){
            $log=Join-Path $suite "disabled-$mode-$bits.log"
            & $loader --prefix $prefix run "C:\tests\disabled$bits\override-disabled-probe$bits.exe" "--$mode" override-marker.dll *> $log
            if($LASTEXITCODE -or (Test-Path -LiteralPath $marker)){throw "Packaged disabled-DLL $mode smoke failed ($bits-bit)"}
            $content=Get-Content -Raw -LiteralPath $log
            if($content -notmatch '(?m)^PASS ' -or $content -match '(?m)^FAIL '){throw "Missing disabled-DLL $mode evidence ($bits-bit)"}
            $checks.Add(@{fixture="disabled-$mode-$bits";passed=$true})
        }
        $log=Join-Path $suite "disabled-startup-$bits.log"
        & $loader --prefix $prefix run "C:\tests\disabled$bits\override-disabled-static$bits.exe" --noop *> $log
        if($LASTEXITCODE -ne 1 -or (Test-Path -LiteralPath $marker) -or (Get-Content -Raw -LiteralPath $log) -notmatch 'disabled DLL required by startup graph: override-marker.dll'){
            throw "Packaged disabled startup import was not rejected before DLL execution ($bits-bit)"
        }
        $checks.Add(@{fixture="disabled-startup-$bits";passed=$true})
        $env:WINEDOWS_OVERRIDE_MARKER=$null
    }
    $env:WINEDLLOVERRIDES='dxgi,d3d11=b;vulkan-1=b'
    $env:WINE_D3D_CONFIG='renderer=vulkan'
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $fixture="wined3d-vulkan-probe$bits.exe"
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory $fixture) -Destination $programs
        $log=Join-Path $suite "vulkan-$bits.log"
        & $loader --prefix $prefix --desktop 'Package,800x600' run "C:\tests\$fixture" --wine --empty-first *> $log
        if($LASTEXITCODE){throw "Packaged WineD3D Vulkan lifecycle smoke failed ($bits-bit)"}
        $content=Get-Content -Raw -LiteralPath $log
        foreach($record in @(@{prefix='D3D11 MODULE ';file='d3d11.dll'},@{prefix='WINE VULKAN MODULE ';file='winevulkan.dll'},@{prefix='WINED3D MODULE ';file='wined3d.dll'})){
            $expected=Join-Path $runtime "builtin\$arch\$($record.file)"
            if($content -notmatch [Regex]::Escape($record.prefix+$expected)){throw "Packaged Vulkan provider mismatch: $($record.file) ($bits-bit)"}
        }
        if($content -match '(?m)^FAIL ' -or $content -notmatch 'PASS complete device/swapchain lifecycle cycle=2' -or $content -notmatch 'PASS physical display mode unchanged' -or $content -notmatch 'PASS real shader work and presentation traversed WineVulkan'){
            throw "Packaged Vulkan rendering/lifecycle evidence missing ($bits-bit)"
        }
        $checks.Add(@{fixture="vulkan-$bits";passed=$true})
    }
    @{passed=$true;archive=[IO.Path]::GetFullPath($Archive);verifiedFiles=$verified;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Package verified: $verified hashes, $($checks.Count) x86/x64 smoke cases. Artifacts: $suite"
    $global:LASTEXITCODE=0
}finally{$env:WINEDLLOVERRIDES=$savedOverrides;$env:WINE_D3D_CONFIG=$savedD3DConfig;$env:WINEDOWS_OVERRIDE_MARKER=$savedMarker}
