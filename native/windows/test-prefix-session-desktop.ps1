param([Parameter(Mandatory)][string]$RuntimeDirectory,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\session-window-fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-session-window-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\session-desktop-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $suite|Out-Null
$rows=[Collections.Generic.List[object]]::new();$processes=[Collections.Generic.List[object]]::new();$complete=$false;$failure=$null
function Check([bool]$ok,[string]$name){$rows.Add(@{name=$name;passed=$ok});if(!$ok){throw $name};Write-Host "PASS $name"}
function Start-Owned([string]$label,[string]$exe,[string[]]$arguments){
    $si=[Diagnostics.ProcessStartInfo]::new($exe);$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    foreach($key in @($si.Environment.Keys)){if($key -like 'WINEDOWS_*' -or $key -eq 'WINEPREFIX' -or $key -eq 'WINEDLLOVERRIDES'){$si.Environment.Remove($key)|Out-Null}}
    foreach($arg in $arguments){$si.ArgumentList.Add($arg)}
    $p=[Diagnostics.Process]::Start($si);$entry=@{p=$p;label=$label;out=$p.StandardOutput.ReadToEndAsync();err=$p.StandardError.ReadToEndAsync();done=$false};$processes.Add($entry);return $entry
}
function Finish($entry,[int]$code){
    $done=$entry.p.WaitForExit(15000);if(!$done){$entry.p.Kill($true);[void]$entry.p.WaitForExit(2000)}
    $actual=if($done){$entry.p.ExitCode}else{124};$drained=$entry.out.Wait(2000) -and $entry.err.Wait(2000)
    if($drained){$entry.out.Result|Set-Content "$suite\$($entry.label).log";$entry.err.Result|Set-Content "$suite\$($entry.label).stderr.log"}
    $entry.done=$true;$ok=$done -and $drained -and $actual -eq $code
    $rows.Add(@{name=$entry.label;passed=$ok;exitCode=$actual;expected=$code;timedOut=(!$done)})
    if(!$ok){throw "$($entry.label) returned $actual expected $code"}
    return $entry.out.Result
}
function Marker([string]$prefix,[string]$token,[string]$phase,$process){
    $file=Join-Path $prefix "drive_c\tests\$token-$phase";$deadline=[DateTime]::UtcNow.AddSeconds(15)
    while([DateTime]::UtcNow -lt $deadline){
        if(Test-Path -LiteralPath $file){
            $text=[IO.File]::ReadAllText($file)
            if($text -match '(?m)^window=(\d+)\r?$'){
                $window=$Matches[1]
                if($text -match '(?m)^desktop=(\d+)\r?$'){return @{window=$window;desktop=$Matches[1]}}
            }
        }
        if($process.p.HasExited){Finish $process 0|Out-Null;throw "Process exited before $phase"};Start-Sleep -Milliseconds 20
    }
    throw "Window marker deadline $phase"
}
function Signal([string]$prefix,[string]$token,[string]$phase){[IO.File]::WriteAllText((Join-Path $prefix "drive_c\tests\$token-$phase"),'signal')}
try{
    $beforeProbe=Start-Owned 'host-before' "$FixtureDirectory\session-window-probe64.exe" @('--host-mode');$before=Finish $beforeProbe 0
    $beforeMode=($before -split "`n"|Where-Object {$_ -like 'HOST *'}).Trim()
    foreach($pair in @(@(32,64),@(64,32))){
        $a=$pair[0];$b=$pair[1];$label="$a-to-$b";$token=[Guid]::NewGuid().ToString('N');$prefix=Join-Path $suite $label
        $init=Start-Owned "$label-init" "$RuntimeDirectory\wine64.exe" @('--prefix',$prefix,'init');Finish $init 0|Out-Null
        $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
        foreach($bits in @(32,64)){Copy-Item "$FixtureDirectory\session-window-probe$bits.exe" "$tests\window$bits.exe"}
        $owner=Start-Owned "$label-owner" "$RuntimeDirectory\wine$a.exe" @('--prefix',$prefix,'--desktop','Shared,640x480','run',"C:\tests\window$a.exe",'owner',$token,'640','480')
        $one=Marker $prefix $token 'owner.ready' $owner
        Signal $prefix $token 'owner.resize';[void](Marker $prefix $token 'owner.resized' $owner)
        $off=Start-Owned "$label-reject-off" "$RuntimeDirectory\wine$b.exe" @('--prefix',$prefix,'--desktop','off','run',"C:\tests\window$b.exe",'forbidden',$token,'800','600')
        Finish $off 1|Out-Null
        Check (!(Test-Path (Join-Path $prefix "drive_c\tests\$token-forbidden.ready"))) "$label explicit desktop off rejected before application execution"
        foreach($variant in @('omitted','explicit')){
            $role="join-$variant";$args=@('--prefix',$prefix)
            if($variant -eq 'explicit'){$args+=@('--desktop','Shared,640x480')}
            $args+=@('run',"C:\tests\window$b.exe",$role,$token,'800','600')
            $joined=Start-Owned "$label-$variant" "$RuntimeDirectory\wine$b.exe" $args
            $two=Marker $prefix $token "$role.ready" $joined
            Check ($two.desktop -eq $one.desktop -and $two.window -ne $one.window) "$label $variant uses existing desktop without replacing owner window"
            foreach($record in @(@{kind='owner';value=$one},@{kind='joined';value=$two})){
                $inspect=Start-Owned "$label-$variant-physical-$($record.kind)" "$FixtureDirectory\session-window-probe64.exe" @('--host',$record.value.window,$one.desktop)
                Finish $inspect 0|Out-Null
            }
            Signal $prefix $token "$role.stop";Finish $joined 43|Out-Null
            Check (!$owner.p.HasExited) "$label $variant join exit preserves desktop owner"
        }
        Signal $prefix $token 'owner.stop';Finish $owner 31|Out-Null
    }
    $afterProbe=Start-Owned 'host-after' "$FixtureDirectory\session-window-probe64.exe" @('--host-mode');$after=Finish $afterProbe 0
    $afterMode=($after -split "`n"|Where-Object {$_ -like 'HOST *'}).Trim();Check ($beforeMode -and $beforeMode -eq $afterMode) 'host display mode unchanged'
    $complete=$true
}catch{$failure=$_.ToString();throw}finally{
    foreach($entry in $processes){try{if(!$entry.p.HasExited){$entry.p.Kill($true);[void]$entry.p.WaitForExit(2000)}}catch{};if(!$entry.done){if($entry.out.Wait(1000)){$entry.out.Result|Set-Content "$suite\$($entry.label).log"};if($entry.err.Wait(1000)){$entry.err.Result|Set-Content "$suite\$($entry.label).stderr.log"}};$entry.p.Dispose()}
    @{passed=$complete -and !($rows|Where-Object {!$_.passed});runtime=$RuntimeDirectory;results=$rows;failure=$failure;scope='Only owned windows; no input injection. Actual unhooked parent checks, independent cross-architecture joins, current virtual mode preservation and physical host mode unchanged.'}|ConvertTo-Json -Depth 6|Set-Content "$suite\result.json"
    Write-Host "Session desktop evidence: $suite"
}
$global:LASTEXITCODE=0
