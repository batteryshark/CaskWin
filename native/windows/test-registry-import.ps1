param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-registry-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\registry-import-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'primary';$sibling=Join-Path $suite 'sibling'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$runtime32=Join-Path $RuntimeDirectory 'wine32.exe'
$token=[Guid]::NewGuid().ToString('N');$clsid='{'+[Guid]::NewGuid().ToString()+'}'
$utf8=[Text.UTF8Encoding]::new($true)
foreach($p in @($prefix,$sibling)){
    & $runtime --prefix $p init
    if($LASTEXITCODE){throw 'Registry import prefix initialization failed'}
    $directory=Join-Path $p 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){
        Copy-Item -LiteralPath "$FixtureDirectory\registry-probe$bits.exe" -Destination (Join-Path $directory "registry$bits.exe")
        foreach($kind in @('machine','user')){Copy-Item -LiteralPath "$FixtureDirectory\registry-com$bits.dll" -Destination (Join-Path $directory "registry-$kind$bits.dll")}
    }
}
function Invoke-Fixture([int]$bits,[string]$mode,[string]$label,[string]$target=$prefix){
    if($mode.StartsWith('--host-')){& "$FixtureDirectory\registry-probe$bits.exe" $mode $token $clsid 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    else{& $runtime --prefix $target run "C:\tests\registry$bits.exe" $mode $token $clsid 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    if($LASTEXITCODE){throw "Registry import fixture failed: $label-$bits (exit $LASTEXITCODE). Evidence: $suite"}
}
function Invoke-Import([string]$file,[string]$view,[string]$loader=$runtime){
    $arguments=@('--prefix',$prefix,'reg','import',$file);if($view){$arguments+=$view}
    & $loader @arguments
    if($LASTEXITCODE){throw "Registry import command failed: $file $view"}
}
foreach($bits in @(32,64)){Invoke-Fixture $bits '--host-absent' 'host-before';Invoke-Fixture $bits '--host-import-routes' 'native-import-routes'}
foreach($bits in @(32,64)){
    $text=@"
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\Software\WinedowsRegistryTests\$token\Import]
"View"="$bits"

[HKEY_CURRENT_USER\Software\WinedowsRegistryTests\$token\Import]
"Shared"="$bits"

[HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid\InprocServer32]
@="C:\\tests\\registry-machine$bits.dll"
"ThreadingModel"="Both"

[HKEY_CLASSES_ROOT\Winedows.Registry.$token.ImportShared]
@="$bits"

[HKEY_CURRENT_USER\Software\Classes\Winedows.Registry.$token.ImportUserShared]
@="$bits"
"@
    $file=Join-Path $suite "machine view $bits.reg";[IO.File]::WriteAllText($file,$text,$utf8)
    # Exercise the opposite launcher architecture deliberately.
    $loader=if($bits -eq 32){$runtime}else{$runtime32}
    Invoke-Import $file "/reg:$bits" $loader
}
$default=Join-Path $suite 'default.reg'
[IO.File]::WriteAllText($default,"Windows Registry Editor Version 5.00`n[HKEY_LOCAL_MACHINE\Software\WinedowsRegistryTests\$token\DefaultImport]`n`"View`"=`"default64`"`n",$utf8)
Invoke-Import $default '' $runtime32
$physical=Join-Path $suite 'physical-wine.reg'
$nativePath="Machine\Software\WinedowsRegistryTests\$token\WinePhysical".Replace('\','\\')
$narrowPath="Machine\Software\WOW6432Node\WinedowsRegistryTests\$token\WinePhysical".Replace('\','\\')
[IO.File]::WriteAllText($physical,"WINE REGISTRY Version 2`n[$nativePath]`n`"View`"=`"physical64`"`n[$narrowPath]`n`"View`"=`"physical32`"`n",$utf8)
Invoke-Import $physical '/reg:32'
Invoke-Import $physical '/reg:64' $runtime32
$invalid=Join-Path $suite 'invalid-view.reg'
[IO.File]::WriteAllText($invalid,"REGEDIT4`n[HKEY_LOCAL_MACHINE\Software\WinedowsRegistryTests\$token\InvalidImport]`n@=`"should not be imported`"`n",$utf8)
& $runtime --prefix $prefix reg import $invalid '/reg:bogus' 2>&1|Tee-Object -FilePath (Join-Path $suite 'invalid-view.log')
if(!$LASTEXITCODE){throw 'Invalid registry view was accepted'}
foreach($bits in @(32,64)){Invoke-Fixture $bits '--import-check' 'import-check';Invoke-Fixture $bits '--activate-machine' 'activate-machine'}
foreach($bits in @(32,64)){
    $file=Join-Path $suite "user$bits.reg"
    $text="REGEDIT4`n[HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid\InprocServer32]`n@=`"C:\\tests\\registry-user$bits.dll`"`n`"ThreadingModel`"=`"Both`"`n"
    [IO.File]::WriteAllText($file,$text,$utf8)
    $loader=if($bits -eq 32){$runtime}else{$runtime32}
    Invoke-Import $file "/reg:$bits" $loader
}
foreach($bits in @(32,64)){Invoke-Fixture $bits '--activate-user' 'activate-user';Invoke-Fixture $bits '--absent' 'sibling' $sibling;Invoke-Fixture $bits '--host-absent' 'host-after'}
@{passed=$true;architectures=@('x86','x64');windowsViews=@(32,64);sharedClsid=$clsid;winePhysicalLayoutPreserved=$true;defaultView=64;hostAndSiblingIsolated=$true;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $suite 'result.json')
Write-Host "Registry import suite passed: $suite"
$global:LASTEXITCODE=0
