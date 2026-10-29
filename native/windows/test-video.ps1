param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-04'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-video-probe.ps1" -OutputDirectory $RuntimeDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\video-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$sibling=Join-Path $suite 'sibling';$overridden=Join-Path $suite 'override';$defaults=Join-Path $suite 'defaults'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$results=[Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Force -Path $suite|Out-Null
$classes=@('{e79167d7-1b85-4d78-b603-798e0e1a4c67}','{48E2ED0F-98C2-4A37-BED5-166312DDD83F}','{477EC299-1421-4bdd-971F-7CCB933F21AD}','{271C3902-6095-4c45-A22F-20091816EE9E}','{62CE7E72-4C71-4d20-B15D-452831A87D9D}')
function Host-VideoState {
    $records=foreach($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)){
        $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
        try{foreach($clsid in $classes){$key=$root.OpenSubKey("Software\Classes\CLSID\$clsid\InprocServer32");try{
            @{view=$view.ToString();clsid=$clsid;module=if($key){$key.GetValue('',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}else{$null};threading=if($key){$key.GetValue('ThreadingModel')}else{$null}}
        }finally{if($key){$key.Dispose()}}}}finally{$root.Dispose()}
    }
    return ($records|ConvertTo-Json -Depth 5 -Compress)
}
function Init-VideoPrefix([string]$path){
    & $runtime --prefix $path init *> (Join-Path $suite ((Split-Path -Leaf $path)+'-init.log'))
    if($LASTEXITCODE){throw "Video prefix initialization failed: $path"}
    $tests=Join-Path $path 'drive_c\tests';New-Item -ItemType Directory -Force -Path $tests|Out-Null
    foreach($bits in @(32,64)){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "video-probe$bits.exe") -Destination $tests}
}
function Run-VideoCase([string]$label,[string]$path,[int]$bits,[string]$desktop,[string]$mode,[string]$expectReader,[string]$expectedFailure=''){
    $log=Join-Path $suite "$label.log"
    & $runtime --prefix $path --desktop $desktop run "C:\tests\video-probe$bits.exe" $mode 'C:\tests\generated.mp4' *> $log
    $code=$LASTEXITCODE;$content=Get-Content -Raw -LiteralPath $log
    if($expectedFailure){
        if(!$code -or $content -notmatch $expectedFailure){throw "Expected video failure not observed: $label ($code)"}
        $results.Add(@{case=$label;status='expected-failure';exitCode=$code});Write-Host "PASS $label (expected failure)";return
    }
    if($code -or $content -notmatch 'successful native DXVA decode submissions prove hardware use' -or $content -notmatch 'present decoded video frame'){throw "Video case failed: $label ($code)"}
    $object=($content -split "`n"|Where-Object{$_ -like 'SOURCE READER OBJECT *'}|Select-Object -First 1)
    if($expectReader -eq 'wine'){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$expected=Join-Path $RuntimeDirectory "builtin\$arch\mfreadwrite.dll"
        if($object -notmatch [Regex]::Escape($expected)){throw "Wine source reader was not selected: $object"}
    }elseif($object -notmatch '(?i)\\Windows\\(System32|SysWOW64)\\mfreadwrite.dll'){throw "Native source reader was not selected: $object"}
    if($content -notmatch 'PASS physical display mode unchanged'){throw "Display preservation was not confirmed: $label"}
    $results.Add(@{case=$label;status='passed';architecture=$bits;reader=$expectReader;desktop=$desktop;mode=$mode});Write-Host "PASS $label"
}
$before=Host-VideoState;$displayBefore=& (Join-Path $RuntimeDirectory 'display-probe64.exe')|Where-Object{$_ -like 'DISPLAY *'}
$savedOverrides=$env:WINEDLLOVERRIDES;$savedTrace=$env:WINEDOWS_TRACE
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_TRACE=$null
    & (Join-Path $RuntimeDirectory 'video-probe64.exe') --generate (Join-Path $suite 'generated.mp4') *> (Join-Path $suite 'generate.log')
    if($LASTEXITCODE){throw 'Synthetic H.264 generation failed'}
    foreach($path in @($prefix,$sibling,$overridden,$defaults)){Init-VideoPrefix $path}
    foreach($path in @($prefix,$overridden)){Copy-Item -LiteralPath (Join-Path $suite 'generated.mp4') -Destination (Join-Path $path 'drive_c\tests')}
    foreach($bits in @(32,64)){
        & (Join-Path $RuntimeDirectory "video-defaults-probe$bits.exe") --disposable-hive (Join-Path $defaults '.winedows\registry.dat') *> (Join-Path $suite "defaults-$bits.log")
        if($LASTEXITCODE){throw "Video defaults preservation failed: $bits"}
        $results.Add(@{case="defaults-$bits";status='passed'})
    }
    foreach($bits in @(32,64)){foreach($reader in @('native','wine')){
        $provider=if($reader -eq 'wine'){'b'}else{'n'};$env:WINEDLLOVERRIDES="dxgi,d3d11=n;mfreadwrite=$provider"
        foreach($desktop in @('off','Video,800x600')){foreach($async in @($false,$true)){
            $mode=if($async){'--async-lifecycle'}else{'--lifecycle'};if($reader -eq 'wine'){$mode+='-wine'}
            Run-VideoCase "$reader-$bits-$desktop-async$async" $prefix $bits $desktop $mode $reader
        }}
        foreach($async in @($false,$true)){
            $mode=if($async){'--async-drain'}else{'--drain'};if($reader -eq 'wine'){$mode+='-wine'}
            Run-VideoCase "drain-$reader-$bits-async$async" $prefix $bits 'off' $mode $reader
        }
    }}
    foreach($bits in @(32,64)){
        $env:WINEDLLOVERRIDES='dxgi,d3d11=b;mfreadwrite=n'
        Run-VideoCase "wine-graphics-unsupported-$bits" $prefix $bits 'off' '--decode' 'native' 'FAIL hardware decoder profiles available'
        $env:WINEDLLOVERRIDES='dxgi,d3d11=n;mfreadwrite=n'
        Run-VideoCase "sibling-file-isolation-$bits" $sibling $bits 'off' '--decode' 'native' 'FAIL open H.264 source inside current namespace'
        $reg=Join-Path $suite "override-$bits.reg"
@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Classes\CLSID\{62CE7E72-4C71-4d20-B15D-452831A87D9D}\InprocServer32]
@="C:\\tests\\missing-private-decoder.dll"
"ThreadingModel"="Both"
'@|Set-Content -LiteralPath $reg
        & $runtime --prefix $overridden reg import $reg "/reg:$bits"
        if($LASTEXITCODE){throw 'Private decoder override setup failed'}
        Run-VideoCase "private-decoder-override-$bits" $overridden $bits 'off' '--decode' 'native' 'FAIL negotiate hardware decoded NV12'
    }
    $after=Host-VideoState;if($before -ne $after){throw 'Host video COM registrations changed'}
    $displayAfter=& (Join-Path $RuntimeDirectory 'display-probe64.exe')|Where-Object{$_ -like 'DISPLAY *'}
    if(!$displayBefore -or $displayBefore -ne $displayAfter){throw 'Physical display mode changed'}
    @{passed=$true;cases=$results;hostRegistrationsUnchanged=$true;hostDisplayPreserved=$true;codec='generated H.264 baseline 160x96 NV12';hardwareProof='successful native DXVA begin/submit/end calls, decoded GPU pixel readback, video-processor conversion and windowed presentation';scope='native Media Foundation H264 decoder with native or Wine source reader; Wine D3D11 hardware decoding unsupported'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Video suite passed: $suite"
}catch{
    @{passed=$false;cases=$results;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw
}finally{$env:WINEDLLOVERRIDES=$savedOverrides;$env:WINEDOWS_TRACE=$savedTrace}
$global:LASTEXITCODE=0
