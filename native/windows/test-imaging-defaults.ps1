param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-imaging-probes.ps1" -OutputDirectory $FixtureDirectory}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$suite=Join-Path $PSScriptRoot ('test-output\imaging-defaults-'+[guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$checks=[Collections.Generic.List[object]]::new()
function Get-HostImagingRegistration {
    $classes=@('{CACAF262-9370-4615-A13B-9F5539DA4C0A}','{389EA17B-5078-4CDE-B6EF-25C15175C751}','{9456A480-E88B-43EA-9E73-0B2D9B71B1CA}')
    $records=foreach($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)){
        $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
        try{foreach($clsid in $classes){$key=$machine.OpenSubKey("Software\Classes\CLSID\$clsid\InprocServer32");try{
            [ordered]@{view=$view.ToString();clsid=$clsid;module=$key.GetValue('',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames);threading=$key.GetValue('ThreadingModel')}
        }finally{if($key){$key.Dispose()}}}}finally{$machine.Dispose()}
    }
    return ($records|ConvertTo-Json -Depth 4 -Compress)
}
$before=Get-HostImagingRegistration
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Imaging defaults prefix init failed'}
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
try{
    $images=Join-Path $tests 'images';New-Item -ItemType Directory -Path $images|Out-Null
    & "$FixtureDirectory\d3dx-image-probe64.exe" --generate $images *> (Join-Path $suite 'generate.log')
    $checks.Add(@{fixture='generate';exitCode=$LASTEXITCODE});if($LASTEXITCODE){throw 'WIC sample generation failed'}
    foreach($bits in @(32,64)){
        & "$FixtureDirectory\imaging-defaults-probe$bits.exe" --disposable-hive (Join-Path $prefix '.winedows\registry.dat') *> (Join-Path $suite "defaults-$bits.log")
        $checks.Add(@{fixture="defaults-$bits";exitCode=$LASTEXITCODE});if($LASTEXITCODE){throw "Imaging default preservation fixture failed ($bits-bit)"}
        Copy-Item -LiteralPath "$FixtureDirectory\wic-probe$bits.exe" -Destination $tests
    }
    foreach($bits in @(32,64)){
        & $runtime --prefix $prefix run "C:\tests\wic-probe$bits.exe" --images 'C:\tests\images' *> (Join-Path $suite "public-com-$bits.log")
        $checks.Add(@{fixture="public-com-$bits";exitCode=$LASTEXITCODE});if($LASTEXITCODE){throw "Public WIC COM activation failed after private defaults ($bits-bit)"}
    }
    $after=Get-HostImagingRegistration
    if($before -ne $after){throw 'Host imaging registrations changed'}
    @{passed=$true;hostRegistrationsUnchanged=$true;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Imaging defaults suite passed: $suite";$global:LASTEXITCODE=0
}catch{
    @{passed=$false;error=$_.Exception.Message;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw
}
