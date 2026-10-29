param([switch]$SkipBuild,[string]$RuntimeDirectory='',[string]$ProbeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$ProbeDirectory){$ProbeDirectory=$RuntimeDirectory}
$ProbeDirectory=[IO.Path]::GetFullPath($ProbeDirectory)
if(!$SkipBuild){
    & "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory
    & "$PSScriptRoot\build-media-swap-probe.ps1" -OutputDirectory $ProbeDirectory
}
$session=Join-Path $PSScriptRoot ('test-output\media-swap-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $session 'prefix';$fixtures=Join-Path $session 'fixtures'
New-Item -ItemType Directory -Path $session|Out-Null
python "$PSScriptRoot\make-media-swap-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'Live swap fixture generation failed'}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
function Invoke-Swap([string[]]$arguments,[string]$name,[string]$loader=$runtime,[switch]$ExpectFailure){
    $saved=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{& $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $session "$name.log");$code=$LASTEXITCODE}
    finally{$ErrorActionPreference=$saved}
    if($ExpectFailure){if(!$code){throw "Expected operation rejection: $name"}}
    elseif($code){throw "Live swap operation failed: $name (exit $code)"}
}
$sourceHashes=@{}
foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$sourceHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
$a=Join-Path $fixtures 'disc-a.cue';$b=Join-Path $fixtures 'disc-b.cue';$c=Join-Path $fixtures 'disc-c.iso';$bad=Join-Path $fixtures 'invalid.iso'
Invoke-Swap @('init') 'init'
Invoke-Swap @('drive','mount','D',$a) 'mount-a'
$ini=Join-Path $prefix 'dosdevices\drives.ini';$mappingHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$cacheHashes=@{}
foreach($file in Get-ChildItem -LiteralPath (Join-Path $prefix '.winedows\media') -File -Recurse){$cacheHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$ProbeDirectory\media-swap-probe$bits.exe" -Destination $tests}
foreach($bits in @(32,64)){
    $other=if($bits -eq 32){64}else{32};$mode=if($bits -eq 64){'exhaust'}else{'normal'}
    Invoke-Swap @('run',"C:\tests\media-swap-probe$bits.exe",(Join-Path $tests "media-swap-probe$other.exe"),(Join-Path $RuntimeDirectory "wine$bits.exe"),$a,$b,$c,$bad,$mode) "swap-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    $endpoint=Join-Path $prefix '.winedows\media-session.control'
    if(Test-Path -LiteralPath $endpoint){throw 'Loader shutdown left a live media endpoint behind'}
    Copy-Item -LiteralPath (Join-Path $tests 'expired-controller.bin') -Destination $endpoint
    Invoke-Swap @('drive','swap','D',$b) "stale-endpoint-$bits" -ExpectFailure
    if((Get-Content -Raw -LiteralPath (Join-Path $session "stale-endpoint-$bits.log")) -notmatch 'stale session'){throw 'Stale endpoint failed for the wrong reason'}
    Invoke-Swap @('run',"C:\tests\media-swap-probe$bits.exe",'--initial') "fresh-session-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $mappingHash){throw 'Live replacement changed persistent drive configuration'}
}
foreach($path in $sourceHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sourceHashes[$path]){throw "Original disc fixture changed: $path"}}
foreach($path in $cacheHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $cacheHashes[$path]){throw "Retired immutable cache changed: $path"}}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
@{passed=$true;architectures=@('x86','x64');realLiveSwap=$true;cueToIsoAndBack=$true;exactPcmAndToc=$true;
    oldFileDirectoryDeviceHandles=$true;inheritedAndCrossProcessDuplicateHandles=$true;crossBitnessChildren=$true;
    staleRelativeAndAbsolutePaths=$true;logicalAndPhysicalGeometry=$true;boundedHistoryExhaustion=$true;
    invalidRequestsAndLockedRollback=$true;staleEndpointRejected=$true;controllerShutdown=$true;sessionReset=$true;
    existingMappedPagesPreserved=$true;configurationUnchanged=$true;originalsUnchanged=$true;
    retiredCacheUnchanged=$true;hostDriveListUnchanged=$true;utc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $session 'result.json')
Write-Host "Live media swap suite passed: $session"
$global:LASTEXITCODE=0
