param([switch]$SkipBuild,[string]$RuntimeDirectory,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-registry-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\registry-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'primary';$sibling=Join-Path $suite 'sibling'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$token=[Guid]::NewGuid().ToString('N')
$sharedClsid='{'+[Guid]::NewGuid().ToString()+'}'
$clsids=@{32=$sharedClsid;64=$sharedClsid}
foreach($p in @($prefix,$sibling)){
    & $runtime --prefix $p init
    if($LASTEXITCODE){throw 'Registry prefix initialization failed'}
    $directory=Join-Path $p 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){
        Copy-Item -LiteralPath "$FixtureDirectory\registry-probe$bits.exe" -Destination (Join-Path $directory "registry$bits.exe")
        foreach($kind in @('machine','user')){Copy-Item -LiteralPath "$FixtureDirectory\registry-com$bits.dll" -Destination (Join-Path $directory "registry-$kind$bits.dll")}
    }
}
$failed=[Collections.Generic.List[string]]::new()
function Invoke-RegistryFixture([int]$bits,[string]$mode,[string]$label,[string]$target=$prefix) {
    $arguments=@($mode,$token,$clsids[$bits])
    if($mode.StartsWith('--host-')){& "$FixtureDirectory\registry-probe$bits.exe" @arguments 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    else{& $runtime --prefix $target run "C:\tests\registry$bits.exe" @arguments 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    if($LASTEXITCODE){$failed.Add("$label-$bits (exit $LASTEXITCODE)")}
}
function Invoke-BackendFixture([int]$bits,[string]$mode,[string]$label,[string]$target,[string]$snapshot) {
    if($mode.StartsWith('--host-')){& "$FixtureDirectory\registry-probe$bits.exe" $mode $token $snapshot 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    else{& $runtime --prefix $target run "C:\tests\registry$bits.exe" $mode $token $snapshot 2>&1|Tee-Object -FilePath (Join-Path $suite "$label-$bits.log")}
    if($LASTEXITCODE){$failed.Add("$label-$bits (exit $LASTEXITCODE)")}
}
foreach($bits in @(32,64)){Invoke-RegistryFixture $bits '--host-names' 'native-name-baseline';Invoke-RegistryFixture $bits '--host-route-tests' 'native-route-baseline';Invoke-RegistryFixture $bits '--host-duplicate-view' 'native-duplicate-baseline';Invoke-RegistryFixture $bits '--host-absent' 'host-before'}
if($failed.Count){throw "Native key-name baseline or host isolation precheck failed: $($failed -join ', ')"}
foreach($bits in @(32,64)){
    Invoke-RegistryFixture $bits '--names-classes' 'names-classes'
    Invoke-RegistryFixture $bits '--wow64-views' 'wow64-views'
    Invoke-RegistryFixture $bits '--duplicate-view' 'duplicate-views'
}
foreach($mode in @('seed-machine','activate-machine','seed-user','activate-user')){
    foreach($bits in @(32,64)){Invoke-RegistryFixture $bits "--$mode" $mode}
}
foreach($bits in @(32,64)){
    Invoke-RegistryFixture $bits '--absent' 'sibling' $sibling
    Invoke-RegistryFixture $bits '--host-absent' 'host-after'
}
foreach($bits in @(32,64)){
    # OLE/Rpc are shared Windows branches; keep per-architecture mutation tests
    # in distinct prefixes so both start by exercising actual host discovery.
    $backendPrefix=Join-Path $suite "backend$bits"
    & $runtime --prefix $backendPrefix init
    if($LASTEXITCODE){throw 'Backend prefix initialization failed'}
    $directory=Join-Path $backendPrefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    Copy-Item -LiteralPath "$FixtureDirectory\registry-probe$bits.exe" -Destination (Join-Path $directory "registry$bits.exe")
    $snapshot=Join-Path $directory 'backend.bin'
    Invoke-BackendFixture $bits '--host-backend-snapshot' 'backend-host-before' $backendPrefix $snapshot
    Copy-Item -LiteralPath $snapshot -Destination (Join-Path $sibling "drive_c\tests\backend$bits.bin")
    Invoke-BackendFixture $bits '--backend-check' 'backend-private-override' $backendPrefix 'C:\tests\backend.bin'
    Invoke-BackendFixture $bits '--backend-read' 'backend-sibling-unchanged' $sibling "C:\tests\backend$bits.bin"
    Invoke-BackendFixture $bits '--host-backend-unchanged' 'backend-host-after' $backendPrefix $snapshot
}
@{
    passed=($failed.Count -eq 0);architectures=@('x86','x64');
    coverage=@('KeyNameInformation buffer contracts','HKCR merged views','private COM activation with shared cross-bitness CLSID','explicit and inherited WOW64 views','same-process duplicate views, failures, close-source, protected sources and observed handle reuse','sibling and host absence audits','OLE/Rpc discovery with private overrides and unchanged host/sibling configuration','host COM Classes excluded from backend read-through');
    limitations=@('Cross-process duplicated/inherited registry handles are covered separately by test-registry-transfers.ps1');
    suiteToken=$token;classIds=@{x86=$clsids[32];x64=$clsids[64]};failures=@($failed);utc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $suite 'result.json')
if($failed.Count){throw "Registry suite failed: $($failed -join ', '). Evidence: $suite"}
Write-Host "Registry suite passed: $suite"
$global:LASTEXITCODE=0
