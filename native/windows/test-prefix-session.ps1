param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\session-fixtures'),
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-session-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\prefix-session-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$results=[Collections.Generic.List[object]]::new()
$running=[Collections.Generic.List[object]]::new()
$applicationHandles=[Collections.Generic.List[object]]::new()
$completedSuite=$false
$failure=$null
function Check([bool]$ok,[string]$label){
    $results.Add(@{name=$label;passed=$ok})
    if(!$ok){throw "Session assertion failed: $label"}
    Write-Host "PASS $label"
}
function Start-Owned([string]$name,[string]$exe,[string[]]$arguments){
    $info=[Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    # These are truly independent host launches, not children carrying a
    # previous injected session's environment or object handles.
    foreach($key in @($info.Environment.Keys)){if($key -like 'WINEDOWS_*' -or $key -eq 'WINEPREFIX' -or $key -eq 'WINEDLLOVERRIDES'){$info.Environment.Remove($key)|Out-Null}}
    foreach($arg in $arguments){$info.ArgumentList.Add($arg)}
    $p=[Diagnostics.Process]::Start($info)
    $entry=@{name=$name;process=$p;stdout=$p.StandardOutput.ReadToEndAsync();stderr=$p.StandardError.ReadToEndAsync();finished=$false}
    $running.Add($entry);return $entry
}
function Finish-Owned($entry,[int]$expected=0,[int]$milliseconds=20000,[switch]$Nonzero){
    $p=$entry.process
    $done=$p.WaitForExit($milliseconds)
    if(!$done){$p.Kill($true);[void]$p.WaitForExit(2000)}
    $code=if($done){$p.ExitCode}else{124}
    $out=if($entry.stdout.Wait(2000)){$entry.stdout.Result}else{'stdout drain deadline exceeded';$code=124}
    $err=if($entry.stderr.Wait(2000)){$entry.stderr.Result}else{'stderr drain deadline exceeded';$code=124}
    $out|Set-Content -LiteralPath (Join-Path $suite ($entry.name+'.log'))
    $err|Set-Content -LiteralPath (Join-Path $suite ($entry.name+'.stderr.log'))
    $entry.finished=$true
    $ok=$done -and $code -ne 124 -and $(if($Nonzero){$code -ne 0}else{$code -eq $expected})
    $results.Add(@{name=$entry.name;passed=$ok;exitCode=$code;expected=$(if($Nonzero){'nonzero'}else{$expected});timedOut=(!$done)})
    if(!$ok){throw "Session process failed: $($entry.name) exit=$code expected=$expected"}
    Write-Host "$($entry.name): exit=$code"
    return $out
}
function Invoke-Loader([string]$name,[string]$prefix,[string[]]$arguments,[int]$bits=64,[int]$expected=0,[switch]$Nonzero){
    $e=Start-Owned $name (Join-Path $RuntimeDirectory "wine$bits.exe") (@('--prefix',$prefix)+$arguments)
    Finish-Owned $e -expected $expected -Nonzero:$Nonzero|Out-Null
}
function Initialize-Prefix([string]$prefix,[string]$name){
    Invoke-Loader "$name-init" $prefix @('init')
    $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
    foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\session-probe$bits.exe" -Destination "$tests\session$bits.exe"}
    $removable=Join-Path $prefix 'session-removable';New-Item -ItemType Directory -Path $removable|Out-Null
    [IO.File]::WriteAllText((Join-Path $removable 'state.txt'),'private removable session fixture',[Text.Encoding]::ASCII)
    foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\session-probe$bits.exe" -Destination "$removable\session$bits.exe"}
    Invoke-Loader "$name-removable" $prefix @('drive','map','R',$removable,'removable','SESSION_R')
}
function Start-Probe([string]$name,[string]$prefix,[int]$bits,[string]$role,[string]$token,[int]$exitCode){
    Start-Owned $name (Join-Path $RuntimeDirectory "wine$bits.exe") @('--prefix',$prefix,'run',"C:\tests\session$bits.exe",$role,$token,"$exitCode")
}
function Wait-Marker([string]$prefix,[string]$token,[string]$suffix,$entry){
    $file=Join-Path $prefix "drive_c\tests\$token-$suffix"
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    while([DateTime]::UtcNow -lt $deadline){
        if(Test-Path -LiteralPath $file){
            $data=Get-Content -LiteralPath $file -Raw
            if($data -match '(?m)^pid=(\d+)\r?$'){
                $id=[int]$Matches[1]
                if($data -match '(?m)^media=(Local\\Winedows\.Media\.[a-f0-9]{32})\r?$'){
                    return @{pid=$id;media=$Matches[1];path=$file}
                }
            }
        }
        if($entry.process.HasExited){Finish-Owned $entry -expected 0|Out-Null;throw "Process exited before $suffix"}
        Start-Sleep -Milliseconds 25
    }
    throw "Marker deadline exceeded: $file"
}
function Stop-Probe([string]$prefix,[string]$token,[string]$role){
    [IO.File]::WriteAllText((Join-Path $prefix "drive_c\tests\$token-$role.stop"),'stop')
}
function Audit-Host([string]$name,[string]$token){
    foreach($bits in @(32,64)){
        $p=Start-Owned "$name-$bits" "$FixtureDirectory\session-probe$bits.exe" @('host-absent',$token,'0')
        Finish-Owned $p|Out-Null
    }
}
try {
    foreach($pair in @(@(32,32),@(32,64),@(64,32),@(64,64))){
        $a=$pair[0];$b=$pair[1];$label="$a-to-$b";$token=[Guid]::NewGuid().ToString('N')
        $prefix=Join-Path $suite "$label-primary";$sibling=Join-Path $suite "$label-sibling"
        Initialize-Prefix $prefix $label;Initialize-Prefix $sibling "$label-sibling"
        Audit-Host "$label-host-before" $token
        $primary=Start-Probe "$label-primary" $prefix $a 'primary' $token 37
        $one=Wait-Marker $prefix $token 'primary.ready' $primary
        $primaryApp=[Diagnostics.Process]::GetProcessById($one.pid);$applicationHandles.Add($primaryApp)
        # GetProcessById otherwise opens only a temporary synchronization
        # handle in WaitForExit and may expose a null ExitCode after exit.
        [void]$primaryApp.Handle
        Check ($primaryApp.MainModule.FileName -eq (Join-Path $prefix "drive_c\tests\session$a.exe")) "$label owned primary application identity"
        # A live join must not treat manually edited text hives as import input.
        $edit="`n[Software\\CaskWin\\SessionProbe\\$token]`n`"TextEdit`"=dword:000003e7`n"
        [IO.File]::AppendAllText((Join-Path $prefix 'user.reg'),$edit,[Text.Encoding]::UTF8)
        $secondary=Start-Probe "$label-secondary" $prefix $b 'secondary' $token 53
        $two=Wait-Marker $prefix $token 'secondary.ready' $secondary
        [void](Wait-Marker $prefix $token 'primary.shared' $primary)
        Check ($one.media -eq $two.media) "$label independent launch shares exact media session"
        $other=Start-Probe "$label-sibling-absent" $sibling $b 'absent' $token 0
        $three=Wait-Marker $sibling $token 'absent.ready' $other
        Finish-Owned $other|Out-Null
        Check ($three.media -ne $one.media) "$label sibling media namespace differs"
        Audit-Host "$label-host-live" $token
        $regFile=Join-Path $prefix 'maintenance.reg'
        [IO.File]::WriteAllText($regFile,"Windows Registry Editor Version 5.00`r`n`r`n[HKEY_CURRENT_USER\Software\CaskWin\SessionMaintenance]`r`n`"Marker`"=dword:00000001`r`n")
        foreach($maintenance in @(@('init'),@('reg','export'),@('reg','import',$regFile),@('drive','map','S',(Join-Path $prefix 'session-removable'),'fixed'))){
            Invoke-Loader "$label-reject-$($maintenance[0])-$($maintenance[1])" $prefix $maintenance -bits $b -expected 1
        }
        Invoke-Loader "$label-failed-join" $prefix @('run','C:\tests\does-not-exist.exe') -bits $b -expected 1
        Invoke-Loader "$label-conflicting-desktop" $prefix @('--desktop','Conflict,320x240','run',"C:\tests\session$b.exe",'observer',$token,'19') -bits $b -expected 1
        Invoke-Loader "$label-ejected-executable" $prefix @('run',"R:\session$b.exe",'observer',$token,'19') -bits $b -expected 1
        Check (!(Test-Path -LiteralPath (Join-Path $prefix "drive_c\tests\$token-observer.ready"))) "$label rejected desktop and ejected executable never ran application"
        Check (!$primary.process.HasExited -and !$secondary.process.HasExited) "$label rejected requests preserve active session"
        Stop-Probe $prefix $token 'primary'
        Check ($primaryApp.WaitForExit(10000) -and $primaryApp.ExitCode -eq 37) "$label primary application exits37 first"
        Check (!$primary.process.HasExited -and !$secondary.process.HasExited) "$label owner and joined application survive primary application exit"
        $observer=Start-Probe "$label-observer" $prefix $a 'observer' $token 19
        $four=Wait-Marker $prefix $token 'observer.ready' $observer
        Check ($four.media -eq $two.media) "$label later join retains media namespace"
        Finish-Owned $observer -expected 19|Out-Null
        Stop-Probe $prefix $token 'secondary'
        Finish-Owned $secondary -expected 53|Out-Null
        Finish-Owned $primary -expected 37|Out-Null
        $text=[IO.File]::ReadAllText((Join-Path $prefix 'user.reg'))
        Check ($text.Contains($token) -and $text.Contains('"Primary"=dword:00000065') -and $text.Contains('"Secondary"=dword:000000ca') -and $text.Contains('"Observer"=dword:0000012f') -and !$text.Contains('"TextEdit"')) "$label final registry export contains all live writes without stale text import"
        $persisted=Start-Probe "$label-persisted" $prefix $b 'persisted' $token 0
        $five=Wait-Marker $prefix $token 'persisted.ready' $persisted
        Finish-Owned $persisted|Out-Null
        Check ($five.media -ne $one.media) "$label completed session uses fresh object/media identity"
        Audit-Host "$label-host-after" $token
        Invoke-Loader "$label-maintenance-after" $prefix @('reg','export')
    }
    # Start both loaders without waiting for either application admission.
    foreach($pair in @(@(32,64),@(64,32))){
        $a=$pair[0];$b=$pair[1];$label="race-$a-to-$b";$token=[Guid]::NewGuid().ToString('N')
        $prefix=Join-Path $suite $label;Initialize-Prefix $prefix $label
        $first=Start-Probe "$label-a" $prefix $a 'race-a' $token 41
        $second=Start-Probe "$label-b" $prefix $b 'race-b' $token 42
        $one=Wait-Marker $prefix $token 'race-a.ready' $first;$two=Wait-Marker $prefix $token 'race-b.ready' $second
        Check ($one.media -eq $two.media) "$label racing admissions share one session"
        Stop-Probe $prefix $token 'race'
        Finish-Owned $first -expected 41|Out-Null;Finish-Owned $second -expected 42|Out-Null
        $text=[IO.File]::ReadAllText((Join-Path $prefix 'user.reg'))
        Check ($text.Contains($token) -and $text.Contains('"RaceCount"=dword:00000002')) "$label final race writes exported"
        Audit-Host "$label-host-after" $token
    }
    $completedSuite=$true
} catch {
    $failure=$_.ToString()
    throw
} finally {
    # Only processes explicitly created by this harness are terminated. Their
    # loader jobs own the test descendants; no global process-name searches.
    foreach($entry in $running){
        try{if(!$entry.process.HasExited){$entry.process.Kill($true);[void]$entry.process.WaitForExit(2000)}}catch{}
        if(!$entry.finished){
            if($entry.stdout.Wait(1000)){$entry.stdout.Result|Set-Content -LiteralPath (Join-Path $suite ($entry.name+'.log'))}
            if($entry.stderr.Wait(1000)){$entry.stderr.Result|Set-Content -LiteralPath (Join-Path $suite ($entry.name+'.stderr.log'))}
        }
        $entry.process.Dispose()
    }
    foreach($p in $applicationHandles){$p.Dispose()}
    $passed=($completedSuite -and $results.Count -gt 0 -and !($results|Where-Object {!$_.passed}))
    @{passed=$passed;runtime=$RuntimeDirectory;results=$results;failure=$failure;scope='Independent host launches, shared private registry/named objects/media, owner lifetime, exit codes, admission races, maintenance exclusion and final export; no host registry writes or physical device IO.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Prefix session evidence: $suite"
}
$global:LASTEXITCODE=0
