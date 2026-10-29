param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [string]$RuntimeDirectory,
    [string]$FixtureDirectory,
    [string]$OutputRoot,
    [ValidateSet('idle','render')][string[]]$Workloads=@('idle'),
    [switch]$NoWasapiTelemetry,
    [ValidateSet(0,44100,48000)][int[]]$MasterRates=@(0),
    [switch]$Quantum1024,
    [ValidateRange(5,120)][int]$Seconds=15,
    [ValidateSet('pcm','xact')][string[]]$Modes=@('pcm','xact'),
    [ValidateSet('host-wasapi','host-directsound','host-winmm','prefix-wasapi','prefix-native-directsound','prefix-wine-directsound','prefix-winmm')][string[]]$Cases=@('host-wasapi','host-directsound','host-winmm','prefix-wasapi','prefix-native-directsound','prefix-wine-directsound','prefix-winmm'),
    [switch]$Calibrate,
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'test-output\runtime-04'}
if(!$FixtureDirectory){$FixtureDirectory=Join-Path $PSScriptRoot 'bin'}
$SourceDirectory=[IO.Path]::GetFullPath($SourceDirectory);$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-game-audio-timing-probe.ps1" -OutputDirectory $FixtureDirectory}
if(!$OutputRoot){$OutputRoot=Join-Path $PSScriptRoot 'test-output'}
$suite=Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('game-audio-timing-'+[Guid]::NewGuid().ToString('N'))
$hostApp=Join-Path $suite 'host';$prefix=Join-Path $suite 'prefix';$prefixApp=Join-Path $prefix 'drive_c\diagnostic'
New-Item -ItemType Directory -Path $hostApp|Out-Null
$names=@('FAudio.dll','SDL2.dll')
if($Modes -contains 'xact'){$names+=@('Content\Audio\TheSounds.xgs','Content\Audio\Sound Bank.xsb','Content\Audio\Wave Bank.xwb')}
$hashes=[ordered]@{}
foreach($name in $names){
    $source=Join-Path $SourceDirectory $name;$hashes[$name]=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $destination=Join-Path $hostApp ([IO.Path]::GetFileName($name));Copy-Item -LiteralPath $source -Destination $destination
    if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hashes[$name]){throw "Copied source hash mismatch: $name"}
}
Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'game-audio-timing-probe64.exe') -Destination (Join-Path $hostApp 'timing.exe')
if(@($Cases|Where-Object {$_.StartsWith('prefix-')}).Count){
    & (Join-Path $RuntimeDirectory 'wine64.exe') --prefix $prefix init|Out-Null
    if($LASTEXITCODE){throw 'Cannot initialize timing prefix'}
    New-Item -ItemType Directory -Path $prefixApp|Out-Null
    Get-ChildItem -LiteralPath $hostApp -File|Copy-Item -Destination $prefixApp
}
$checks=[Collections.Generic.List[object]]::new()
function Run-Timing([string]$name,[string]$mode,[int]$rate=0,[int]$stall=0,[string]$workload='idle'){
    $inside=$name.StartsWith('prefix-');$app=if($inside){$prefixApp}else{$hostApp}
    $driver=if($name.EndsWith('wasapi')){'wasapi'}elseif($name.EndsWith('winmm')){'winmm'}else{'directsound'}
    $wine=$name -eq 'prefix-wine-directsound';$expected=if($driver -ne 'directsound'){'-'}elseif($wine){Join-Path $RuntimeDirectory 'builtin\x64\dsound.dll'}else{'native'}
    $label="$name-$mode-$workload";if($rate){$label+="-$rate"};if($Quantum1024){$label+='-q1024'};if($stall){$label+='-calibration'};if($NoWasapiTelemetry){$label+='-unobserved'}
    $psi=[Diagnostics.ProcessStartInfo]::new();$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.WorkingDirectory=$app
    $psi.FileName=if($inside){Join-Path $RuntimeDirectory 'wine64.exe'}else{Join-Path $app 'timing.exe'}
    $arguments=@($mode,"$Seconds",$label,$driver,$expected,"$stall","$rate",[int]$Quantum1024.IsPresent,$workload,[int](!$NoWasapiTelemetry))
    if($inside){$arguments=@('--prefix',$prefix,'run','C:\diagnostic\timing.exe')+$arguments}
    foreach($arg in $arguments){$psi.ArgumentList.Add($arg)}
    # Environment belongs only to this child. Avoid inheriting an unrelated
    # shell's audio tweaks or enabling verbose per-operation tracing.
    foreach($key in @('SDL_AUDIO_SAMPLES','SDL_AUDIO_FREQUENCY','SDL_AUDIO_CHANNELS','SDL_AUDIO_DEVICE_NAME','FAUDIO_FORCE_DEFAULT_DEVICEID','WINEDOWS_TRACE')){$psi.Environment.Remove($key)|Out-Null}
    $psi.Environment['SDL_AUDIODRIVER']=$driver
    $psi.Environment['WINEDLLOVERRIDES']=if($wine){'dsound=b'}else{'dsound=n'}
    $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=!$process.WaitForExit(($Seconds+25)*1000)
    if($timedOut){$process.Kill($true);$process.WaitForExit()}
    $stdout.Result|Set-Content -LiteralPath (Join-Path $suite "$label.log");$stderr.Result|Set-Content -LiteralPath (Join-Path $suite "$label.stderr.log")
    $result=Join-Path $app "$label.json";$record=if(Test-Path -LiteralPath $result){Get-Content -Raw -LiteralPath $result|ConvertFrom-Json}else{$null}
    $checks.Add([ordered]@{case=$label;exitCode=$process.ExitCode;timedOut=$timedOut;result=$record})
    Write-Host "$label exit=$($process.ExitCode) timeout=$timedOut"
    if($record){Write-Host "  $($record.classification): frameRateRatio=$($record.callbackFrameRateRatio), callbacks=$($record.callbacks), maxGap=$($record.gapMaxMs)ms, maxCallback=$($record.callbackMaxMs)ms"}
}
try{
    foreach($workload in $Workloads){foreach($rate in $MasterRates){foreach($mode in $Modes){foreach($case in $Cases){Run-Timing $case $mode $rate 0 $workload}}}}
    if($Calibrate){Run-Timing 'host-wasapi' 'pcm' $MasterRates[0] 120 'idle'}
    foreach($name in $names){if((Get-FileHash -LiteralPath (Join-Path $SourceDirectory $name) -Algorithm SHA256).Hash -ne $hashes[$name]){throw "Original source changed during diagnostics: $name"}}
    $failed=@($checks|Where-Object {$_.exitCode -ne 0 -or $_.timedOut -or !$_.result -or !$_.result.collectionSucceeded})
    $anomalies=@($checks|Where-Object {$_.result -and !$_.result.injectedStallMs -and (!$_.result.progressWithinTolerance -or $_.result.callbacksOverQuantum -or $_.result.gapsOver4xQuantum)}|ForEach-Object {$_.case})
    @{completed=$true;allCollectionsSucceeded=($failed.Count -eq 0);anomalies=$anomalies;sourceDirectory=$SourceDirectory;runtimeDirectory=$RuntimeDirectory;sourceHashes=$hashes;originalsUnchanged=$true;checks=@($checks);capturePerformed=$false;audioOutputForcedSilent=$true;limitations=@('Successful collection does not establish absence of hardware underruns or audible game glitches','Callback metrics describe the application-facing SDL render callback, including backend buffering/conversion','Real title cue uses an in-memory wave bank and bounded60Hz DoWork cadence, not the full game workload','Generated offscreen rendering does not reproduce game scene or Present pacing');utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Timing evidence: $suite"
    if($anomalies.Count){Write-Host "Timing anomalies require interpretation: $($anomalies -join ', ')"}
    if($failed.Count){throw "Unavailable or failed timing cases: $($failed.case -join ', ')"}
}catch{
    if(!(Test-Path -LiteralPath (Join-Path $suite 'result.json'))){@{completed=$false;error=$_.Exception.Message;sourceHashes=$hashes;checks=@($checks)}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $suite 'result.json')}
    $global:LASTEXITCODE=1
    throw
}
$global:LASTEXITCODE=0
