param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-05'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\video-av-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $suite|Out-Null
$fixtures=if($SkipBuild){$RuntimeDirectory}else{Join-Path $suite 'fixtures'}
if(!$SkipBuild){& "$PSScriptRoot\build-av-probe.ps1" -OutputDirectory $fixtures;if($LASTEXITCODE){throw 'AV fixture build failed'}}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$results=[Collections.Generic.List[object]]::new()
$prefix=Join-Path $suite 'prefix';$defaults=Join-Path $suite 'defaults'
$classes=@('{e79167d7-1b85-4d78-b603-798e0e1a4c67}','{48E2ED0F-98C2-4A37-BED5-166312DDD83F}','{477EC299-1421-4bdd-971F-7CCB933F21AD}','{271C3902-6095-4c45-A22F-20091816EE9E}','{62CE7E72-4C71-4d20-B15D-452831A87D9D}','{32D186A7-218F-4C75-8876-DD77273A8999}','{88753B26-5B24-49BD-B2E7-0C445C78C982}')
function Host-AVState {
    $paths=@($classes|ForEach-Object{"Software\Classes\CLSID\$_\InprocServer32"})
    $paths+=@('Software\Classes\MediaFoundation\Transforms\32d186a7-218f-4c75-8876-dd77273a8999','Software\Classes\MediaFoundation\Transforms\88753b26-5b24-49bd-b2e7-0c445c78c982')
    $records=foreach($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)){
        $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
        try{foreach($path in $paths){$key=$root.OpenSubKey($path);try{
            $values=if($key){foreach($name in ($key.GetValueNames()|Sort-Object)){@{name=$name;kind=$key.GetValueKind($name).ToString();value=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}}}
            @{view=$view.ToString();path=$path;exists=($null-ne$key);values=@($values)}
        }finally{if($key){$key.Dispose()}}}}finally{$root.Dispose()}
    }
    return ($records|ConvertTo-Json -Depth 6 -Compress)
}
function Init-AVPrefix([string]$path){
    & $runtime --prefix $path init *> (Join-Path $suite ((Split-Path -Leaf $path)+'-init.log'))
    if($LASTEXITCODE){throw "AV prefix initialization failed: $path"}
    $tests=Join-Path $path 'drive_c\tests';New-Item -ItemType Directory -Force -Path $tests|Out-Null
    foreach($bits in @(32,64)){Copy-Item -LiteralPath (Join-Path $fixtures "av-probe$bits.exe") -Destination $tests}
    Copy-Item -LiteralPath (Join-Path $suite 'generated-av.mp4') -Destination $tests
}
function Run-AVCase([string]$label,[string]$path,[int]$bits,[string]$reader,[string]$mode,[string]$expectedFailure=''){
    $provider=if($reader -eq 'wine'){'b'}else{'n'};$env:WINEDLLOVERRIDES="dxgi,d3d11=n;mfreadwrite=$provider"
    $log=Join-Path $suite "$label.log"
    & $runtime --prefix $path --desktop off run "C:\tests\av-probe$bits.exe" "--$mode" 'C:\tests\generated-av.mp4' *> $log
    $code=$LASTEXITCODE;$content=Get-Content -Raw -LiteralPath $log
    if($expectedFailure){if(!$code -or $content -notmatch $expectedFailure){throw "Expected AV failure not observed: $label ($code)"};$results.Add(@{case=$label;status='expected-failure';exitCode=$code});Write-Host "PASS $label (expected failure)";return}
    if($code -or $content -notmatch 'real native DXVA submissions accompany converted video' -or $content -notmatch 'both streams reach EOF' -or $content -notmatch 'AAC decoded channel preserves generated tone and level'){throw "AV case failed: $label ($code)"}
    $object=($content -split "`n"|Where-Object{$_ -like 'SOURCE READER OBJECT *'}|Select-Object -First 1)
    if($reader -eq 'wine'){$arch=if($bits -eq 32){'x86'}else{'x64'};$expected=Join-Path $RuntimeDirectory "builtin\$arch\mfreadwrite.dll";if($object -notmatch [Regex]::Escape($expected)){throw "Wine reader binding failed: $object"}}
    elseif($object -notmatch '(?i)\\Windows\\(System32|SysWOW64)\\mfreadwrite.dll'){throw "Native reader binding failed: $object"}
    foreach($module in @('msmpeg2vdec.dll','msvproc.dll','MSAudDecMFT.dll')){if($content -notmatch ('(?im)^AV TRANSFORM .*module=.*\\Windows\\(System32|SysWOW64)\\'+[Regex]::Escape($module))){throw "Actual AV transform missing: $module"}}
    if($content -notmatch 'AV HARDWARE video=(\d+) pcm_frames=(\d+) begin=(\d+) submit=(\d+) end=(\d+)'){throw 'AV counters missing'}
    $results.Add(@{case=$label;status='passed';architecture=$bits;reader=$reader;mode=$mode;videoFrames=[int]$Matches[1];pcmFrames=[long]$Matches[2];dxvaSubmissions=[int]$Matches[4]});Write-Host "PASS $label"
}
$savedOverrides=$env:WINEDLLOVERRIDES;$savedTrace=$env:WINEDOWS_TRACE;$before=Host-AVState
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_TRACE=$null
    & (Join-Path $fixtures 'av-probe64.exe') --generate (Join-Path $suite 'generated-av.mp4') *> (Join-Path $suite 'generate.log');if($LASTEXITCODE){throw 'Synthetic audiovisual MP4 generation failed'}
    Init-AVPrefix $prefix;Init-AVPrefix $defaults
    foreach($bits in @(32,64)){& (Join-Path $fixtures "video-defaults-probe$bits.exe") --disposable-hive (Join-Path $defaults '.winedows\registry.dat') *> (Join-Path $suite "defaults-$bits.log");if($LASTEXITCODE){throw "AV defaults preservation failed: $bits"};$results.Add(@{case="defaults-$bits";status='passed'})}
    foreach($bits in @(32,64)){foreach($reader in @('native','wine')){foreach($mode in @('lifecycle','async-lifecycle','drain','async-drain','async-queued','async-selection')){Run-AVCase "$reader-$bits-$mode" $prefix $bits $reader $mode}}}
    foreach($item in @(@{name='aac';id='{32D186A7-218F-4C75-8876-DD77273A8999}';failure='FAIL negotiate AAC decoder to stereo PCM'},@{name='processor';id='{88753B26-5B24-49BD-B2E7-0C445C78C982}';failure='FAIL negotiate decoder and RGB color processor'})){
        $overridden=Join-Path $suite ('override-'+$item.name);Init-AVPrefix $overridden
        $reg=Join-Path $suite ($item.name+'.reg')
        @('Windows Registry Editor Version 5.00','',('[HKEY_CURRENT_USER\Software\Classes\CLSID\'+$item.id+'\InprocServer32]'),'@="C:\\tests\\missing-private-transform.dll"','"ThreadingModel"="Both"')|Set-Content -LiteralPath $reg
        foreach($bits in @(32,64)){& $runtime --prefix $overridden reg import $reg "/reg:$bits" *> (Join-Path $suite "import-$($item.name)-$bits.log");if($LASTEXITCODE){throw 'Private transform override import failed'};foreach($reader in @('native','wine')){Run-AVCase "override-$($item.name)-$reader-$bits" $overridden $bits $reader 'decode' $item.failure}}
    }
    if($before -ne (Host-AVState)){throw 'Host AV registrations changed'}
    @{passed=$true;cases=$results;hostRegistrationsUnchanged=$true;generatedMedia='H264 baseline 160x96 at12fps; AAC stereo48000Hz independently generated440/660Hz tones';scope='actual Wine/native source reader, native AAC/H264 decoders and color processor; native D3D11 DXVA submission and RGB pixel readback; no audio capture or audio-device playback'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Audiovisual suite passed: $suite"
}catch{@{passed=$false;cases=$results;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw}
finally{$env:WINEDLLOVERRIDES=$savedOverrides;$env:WINEDOWS_TRACE=$savedTrace}
$global:LASTEXITCODE=0
