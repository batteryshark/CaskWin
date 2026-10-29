param([switch]$SkipBuild,[string]$RuntimeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){
    & "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory
    & "$PSScriptRoot\build-media-notifications-probe.ps1" -OutputDirectory $RuntimeDirectory
}
$session=Join-Path $PSScriptRoot ('test-output\media-notifications-'+[Guid]::NewGuid().ToString('N'))
$fixtures=Join-Path $session 'fixtures';New-Item -ItemType Directory -Path $session|Out-Null
python "$PSScriptRoot\make-media-swap-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'Notification media fixture generation failed'}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$a=Join-Path $fixtures 'disc-a.cue';$b=Join-Path $fixtures 'disc-b.cue';$bad=Join-Path $fixtures 'invalid.iso'
$hashes=@{};foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$hashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
function Invoke-Setup([string]$prefix,[string[]]$arguments){
    & $runtime --prefix $prefix @arguments *> (Join-Path $session 'setup-last.log')
    if($LASTEXITCODE){throw "Notification setup failed: $arguments"}
}
function Start-Hidden([string]$exe,[string[]]$arguments,[string]$name){
    $quoted=@($arguments|ForEach-Object {'"'+$_+'"'})
    Start-Process -FilePath $exe -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $session "$name.log") -RedirectStandardError (Join-Path $session "$name.err")
}
function Wait-File([string]$path){
    $end=[DateTime]::UtcNow.AddSeconds(10)
    while([DateTime]::UtcNow -lt $end){if(Test-Path -LiteralPath $path){return};Start-Sleep -Milliseconds 20}
    throw "Listener did not become ready: $path"
}
function Stop-Listener($process,[string]$path){
    if(!$process){return};[IO.File]::WriteAllText($path,'1')
    if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id -Force;throw 'Owned notification listener did not exit'}
    if($process.ExitCode){throw "Owned notification listener failed: $($process.ExitCode)"}
}
$hostLog=Join-Path $session 'host-events.log';$hostReady=Join-Path $session 'host.ready';$hostStop=Join-Path $session 'host.stop'
$otherPrefix=Join-Path $session 'other-prefix';Invoke-Setup $otherPrefix @('init')
$otherTests=Join-Path $otherPrefix 'drive_c\tests';New-Item -ItemType Directory -Path $otherTests|Out-Null
Copy-Item -LiteralPath "$RuntimeDirectory\media-notifications-probe64.exe" -Destination $otherTests
$otherLog=Join-Path $otherTests 'other-events.log';$otherReady=Join-Path $otherTests 'other.ready';$otherStop=Join-Path $otherTests 'other.stop'
$hostProcess=$null;$otherProcess=$null;$cases=@()
try{
    $hostProcess=Start-Hidden "$RuntimeDirectory\media-notifications-probe64.exe" @('--listener',$hostLog,$hostReady,$hostStop) 'host-listener'
    $otherProcess=Start-Hidden $runtime @('--prefix',$otherPrefix,'run','C:\tests\media-notifications-probe64.exe','--listener','C:\tests\other-events.log','C:\tests\other.ready','C:\tests\other.stop') 'other-prefix-listener'
    Wait-File $hostReady;Wait-File $otherReady
    foreach($desktop in @($false,$true)){
        foreach($bits in @(32,64)){
            $mode=if($desktop){'desktop'}else{'normal'};$name="$mode-$bits";$prefix=Join-Path $session $name
            Invoke-Setup $prefix @('init');Invoke-Setup $prefix @('drive','mount','D',$a)
            $ini=Join-Path $prefix 'dosdevices\drives.ini';$iniHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
            $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
            Copy-Item -LiteralPath "$RuntimeDirectory\media-notifications-probe32.exe","$RuntimeDirectory\media-notifications-probe64.exe" -Destination $tests
            $other=if($bits -eq 32){64}else{32};$arguments=@('--prefix',$prefix)
            if($desktop){$arguments+=@('--desktop','Notify,320x200')}
            $arguments+=@('run',"C:\tests\media-notifications-probe$bits.exe",'--exercise',"C:\tests\media-notifications-probe$other.exe",(Join-Path $RuntimeDirectory "wine$bits.exe"),$b,$bad)
            $process=Start-Hidden (Join-Path $RuntimeDirectory "wine$bits.exe") $arguments $name
            if(!$process.WaitForExit(45000)){Stop-Process -Id $process.Id -Force;throw "Notification fixture timed out: $name"}
            Get-Content -LiteralPath (Join-Path $session "$name.log")|Write-Host
            if($process.ExitCode){throw "Notification fixture failed: $name (exit $($process.ExitCode))"}
            if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $iniHash){throw 'Notifications changed persistent drive configuration'}
            if((Get-Item -LiteralPath $hostLog).Length -or (Get-Item -LiteralPath $otherLog).Length){throw 'A host or other-prefix window received a virtual media message'}
            $architecture=if($bits -eq 32){'x86'}else{'x64'}
            $cases+=@{architecture=$architecture;virtualDesktop=$desktop;eventsPerParentAndChild=30;passed=$true}
        }
    }
}finally{
    try{Stop-Listener $otherProcess $otherStop}finally{Stop-Listener $hostProcess $hostStop}
}
foreach($bits in @(32,64)){
    $other=if($bits -eq 32){64}else{32};$folder=Join-Path $session "service-$bits";New-Item -ItemType Directory -Path $folder|Out-Null
    $service=Start-Hidden (Join-Path $RuntimeDirectory "media-notifications-service-probe$bits.exe") @((Join-Path $session "normal-$bits"),(Join-Path $RuntimeDirectory "media-notifications-probe$other.exe"),$folder) "service-$bits"
    if(!$service.WaitForExit(15000)){Stop-Process -Id $service.Id -Force;throw "Notification service fixture timed out ($bits)"}
    Get-Content -LiteralPath (Join-Path $session "service-$bits.log")|Write-Host
    if($service.ExitCode){throw "Notification service fixture failed ($bits)"}
    if((Get-Content -Raw -LiteralPath (Join-Path $session "service-$bits.err")) -notmatch 'skipped 5000 transitions'){throw 'Expected explicit overflow diagnostic missing'}
}
foreach($path in $hashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$path]){throw "Original media changed: $path"}}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
@{passed=$true;cases=$cases;hostNotifications=0;otherPrefixNotifications=0;orderedVolumePayloads=$true;
    childAndMessageOnlyControlsSilent=$true;noopsAndFailedChangesSilent=$true;callbackMayLoadMedia=$true;
    delayedPayloadAfterTimeout=$true;sourceAndPersistentConfigurationUnchanged=$true;hostDriveListUnchanged=$true;
    actualBacklogGapResync=$true;counterWrap=$true;initialAttachSilent=$true;boundedStopDuringHungHandler=$true;jobHandleLifetime=$true;
    utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $session 'result.json')
Write-Host "Media notifications suite passed: $session"
$global:LASTEXITCODE=0
