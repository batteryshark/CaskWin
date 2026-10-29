param([Parameter(Mandatory)][string]$RuntimeDirectory,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\session-fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-session-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\session-lifetime-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$checks=[Collections.Generic.List[object]]::new();$running=[Collections.Generic.List[object]]::new();$apps=[Collections.Generic.List[object]]::new()
$complete=$false;$failure=$null
function Check([bool]$ok,[string]$name){$checks.Add(@{name=$name;passed=$ok});if(!$ok){throw $name};Write-Host "PASS $name"}
function Start-Owned([string]$name,[string]$exe,[string[]]$arguments){
    $si=[Diagnostics.ProcessStartInfo]::new($exe);$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    foreach($k in @($si.Environment.Keys)){if($k -like 'WINEDOWS_*' -or $k -eq 'WINEPREFIX' -or $k -eq 'WINEDLLOVERRIDES'){$si.Environment.Remove($k)|Out-Null}}
    foreach($a in $arguments){$si.ArgumentList.Add($a)}
    $p=[Diagnostics.Process]::Start($si);$e=@{name=$name;p=$p;stdout=$p.StandardOutput.ReadToEndAsync();stderr=$p.StandardError.ReadToEndAsync();done=$false};$running.Add($e);return $e
}
function Finish($e,[int]$expected=0,[switch]$AnyExit){
    $exited=$e.p.WaitForExit(15000);if(!$exited){$e.p.Kill($true);[void]$e.p.WaitForExit(2000)}
    $code=if($exited){$e.p.ExitCode}else{124}
    $drained=$e.stdout.Wait(2000) -and $e.stderr.Wait(2000)
    if($drained){$e.stdout.Result|Set-Content "$suite\$($e.name).log";$e.stderr.Result|Set-Content "$suite\$($e.name).stderr.log"}
    $e.done=$true
    $ok=$exited -and $drained -and ($AnyExit -or $code -eq $expected)
    $checks.Add(@{name=$e.name;passed=$ok;exitCode=$code;expected=$(if($AnyExit){'record-only'}else{$expected});timedOut=(!$exited)})
    if(!$ok){throw "$($e.name) exited $code, expected $expected"};Write-Host "$($e.name) exit=$code"
}
function Init([string]$prefix,[string]$label){
    $p=Start-Owned "$label-init" "$RuntimeDirectory\wine64.exe" @('--prefix',$prefix,'init');Finish $p
    $dir=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $dir|Out-Null
    foreach($bits in @(32,64)){Copy-Item "$FixtureDirectory\session-job-probe$bits.exe" "$dir\job$bits.exe"}
}
function Launch([string]$label,[string]$prefix,[int]$bits,[string]$role,[string]$token,[int]$code,[int]$child=32,[switch]$Outer){
    $args=@('--prefix',$prefix,'run',"C:\tests\job$bits.exe",$role,$token,"$code","C:\tests\job$child.exe")
    if($Outer){Start-Owned $label "$FixtureDirectory\session-job-probe$bits.exe" (@('--outer-job',"$RuntimeDirectory\wine$bits.exe")+$args)}
    else{Start-Owned $label "$RuntimeDirectory\wine$bits.exe" $args}
}
function Marker([string]$prefix,[string]$token,[string]$role,$loader){
    $path=Join-Path $prefix "drive_c\tests\$token-$role.ready";$deadline=[DateTime]::UtcNow.AddSeconds(15)
    while([DateTime]::UtcNow -lt $deadline){
        if(Test-Path $path){$text=[IO.File]::ReadAllText($path);if($text -match '(?m)^pid=(\d+)\r?$'){
            $id=[int]$Matches[1];if($text -match '(?m)^media=(Local\\Winedows\.Media\.[0-9a-f]{32})\r?$'){
                $media=$Matches[1];$p=[Diagnostics.Process]::GetProcessById($id);[void]$p.Handle;$apps.Add($p)
                Check ($p.MainModule.FileName -like "$prefix\drive_c\tests\job*.exe") "owned application identity $role"
                return @{process=$p;media=$media}
            }
        }}
        if($loader.p.HasExited){Finish $loader;throw "loader exited before $role marker"};Start-Sleep -Milliseconds 20
    }
    throw "marker deadline $role"
}
function Release([string]$prefix,[string]$token,[string]$role){[IO.File]::WriteAllText((Join-Path $prefix "drive_c\tests\$token-$role.stop"),'release')}
try {
    foreach($pair in @(@(32,64),@(64,32))){
        $a=$pair[0];$b=$pair[1]
        foreach($mode in @('kill-joiner','kill-owner','descendant','outer-jobs')){
            $label="$mode-$a-to-$b";$prefix=Join-Path $suite $label;$token=[Guid]::NewGuid().ToString('N');Init $prefix $label
            $outer=$mode -eq 'outer-jobs'
            $owner=Launch "$label-owner" $prefix $a 'owner' $token 31 -Outer:$outer
            $one=Marker $prefix $token 'owner' $owner
            if($mode -eq 'descendant'){
                $joined=Launch "$label-tree" $prefix $b 'tree' $token 47 -child $a
                $two=Marker $prefix $token 'tree' $joined;$leaf=Marker $prefix $token 'leaf' $joined
                Check ($one.media -eq $two.media -and $one.media -eq $leaf.media) "$label descendant shares session"
                Release $prefix $token 'tree';Check ($two.process.WaitForExit(5000) -and $two.process.ExitCode -eq 47) "$label joined root exits47"
                Check (!$leaf.process.HasExited -and !$joined.p.HasExited) "$label joined loader waits descendant after root exit"
                Release $prefix $token 'owner';Check ($one.process.WaitForExit(5000) -and $one.process.ExitCode -eq 31) "$label first owner application exits31"
                Check (!$owner.p.HasExited -and !$joined.p.HasExited -and !$leaf.process.HasExited) "$label both loaders survive for descendant"
                Release $prefix $token 'leaf';Finish $joined 47;Finish $owner 31
                $text=[IO.File]::ReadAllText((Join-Path $prefix 'user.reg'));Check ($text.Contains('"leafDone"=dword:00000001')) "$label final export includes descendant completion"
            } else {
                $joined=Launch "$label-joined" $prefix $b 'joined' $token 43 -Outer:$outer
                if($outer){
                    Finish $joined 1
                    Check (!(Test-Path (Join-Path $prefix "drive_c\tests\$token-joined.ready"))) "$label unrelated host hierarchy rejected before app execution"
                    Check (!$one.process.HasExited -and !$owner.p.HasExited) "$label incompatible join leaves first application intact"
                    Release $prefix $token 'owner';Finish $owner 31
                } else {
                    $two=Marker $prefix $token 'joined' $joined;Check ($one.media -eq $two.media) "$label independent applications share session"
                    if($mode -eq 'kill-joiner'){
                        $joined.p.Kill();Finish $joined -AnyExit
                        Check ($two.process.WaitForExit(5000)) "$label joined app dies when only its loader is killed"
                        Check (!$owner.p.HasExited -and !$one.process.HasExited) "$label owner remains alive"
                        $observer=Launch "$label-observer" $prefix $a 'inspect' $token 17;Finish $observer 17
                        Release $prefix $token 'owner';Finish $owner 31
                    } else {
                        $owner.p.Kill();Finish $owner -AnyExit
                        Check ($one.process.WaitForExit(5000) -and $two.process.WaitForExit(5000)) "$label owner loss terminates both application trees"
                        Finish $joined -AnyExit
                        $repair=Start-Owned "$label-recover" "$RuntimeDirectory\wine64.exe" @('--prefix',$prefix,'reg','export');Finish $repair
                    }
                }
            }
        }
    }
    $complete=$true
} catch {$failure=$_.ToString();throw} finally {
    foreach($e in $running){
        try{if(!$e.p.HasExited){$e.p.Kill($true);[void]$e.p.WaitForExit(2000)}}catch{}
        if(!$e.done){if($e.stdout.Wait(1000)){$e.stdout.Result|Set-Content "$suite\$($e.name).log"};if($e.stderr.Wait(1000)){$e.stderr.Result|Set-Content "$suite\$($e.name).stderr.log"}}
        $e.p.Dispose()
    }
    foreach($p in $apps){$p.Dispose()}
    @{passed=$complete -and !($checks|Where-Object {!$_.passed});runtime=$RuntimeDirectory;checks=$checks;failure=$failure;scope='Only owned process trees are terminated; no UI, physical input or host registry writes. Unrelated host-job joins must fail without breakaway.'}|ConvertTo-Json -Depth 6|Set-Content "$suite\result.json"
    Write-Host "Session lifecycle evidence: $suite"
}
$global:LASTEXITCODE=0
