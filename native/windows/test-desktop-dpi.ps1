param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-08'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\dpi-08\fixtures'),[switch]$SkipBuild,[switch]$AllowKnownRegression,[switch]$Trace)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-dpi-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\dpi-08\geometry-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$results=[Collections.Generic.List[object]]::new()
function Invoke-DpiCase([string]$Name,[string]$Program,[string[]]$Arguments){
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Program;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in $Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timeout=!$process.WaitForExit(20000);if($timeout){$process.Kill($true);$process.WaitForExit()}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$process.Dispose()
    $text|Set-Content -LiteralPath "$output\$Name.log";$errors|Set-Content -LiteralPath "$output\$Name.stderr.log"
    $events=@($text -split "`r?`n"|Where-Object {$_ -like '{*'}|ForEach-Object {$_|ConvertFrom-Json})
    $summary=$events|Where-Object {$_.kind -eq 'summary'}|Select-Object -Last 1
    $traceLines=@($errors -split "`r?`n"|Where-Object {$_ -like 'WINEDOWS_DESKTOP *'})
    $traceValid=$traceLines.Count -le 128 -and ((!$Trace -or $Name -notlike 'prefix-*') -or $traceLines.Count -gt 0)
    $results.Add(@{name=$Name;exitCode=$code;timedOut=$timeout;summary=$summary;events=$events;traceRecords=$traceLines.Count;traceValid=$traceValid})
    Write-Host "$Name exit=$code regressions=$($summary.regressions) collectionErrors=$($summary.collectionErrors)"
}
$saved=$env:WINEDLLOVERRIDES;$savedTrace=$env:WINEDOWS_TRACE_DESKTOP
try {
    $env:WINEDLLOVERRIDES=$null
    $env:WINEDOWS_TRACE_DESKTOP=if($Trace){'1'}else{$null}
    & $runtime --prefix $prefix init|Out-Host;if($LASTEXITCODE){throw 'DPI prefix initialization failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){foreach($mode in @('unaware','pm')){
        $file="desktop-dpi-$mode$bits.exe";$expected=if($mode -eq 'pm'){'2'}else{'0'}
        Copy-Item -LiteralPath "$FixtureDirectory\$file" -Destination "$directory\$file"
        foreach($scenario in @('hidden-popup','visible-popup','hidden-overlapped')){
            Invoke-DpiCase "host-$mode-$bits-$scenario" "$FixtureDirectory\$file" @($expected,'host',$scenario)
            Invoke-DpiCase "prefix-$mode-$bits-$scenario" $runtime @('--prefix',$prefix,'--desktop','Dpi,1280x720','run',"C:\tests\$file",$expected,'virtual',$scenario)
        }
    }}
    $collected=!($results|Where-Object {$_.timedOut -or !$_.summary -or $_.summary.collectionErrors -ne 0 -or !$_.traceValid})
    $passed=$collected -and !($results|Where-Object {$_.exitCode -ne 0})
    @{completed=$true;allCollectionsSucceeded=$collected;geometryAndAwarenessPreserved=$passed;runtime=$RuntimeDirectory;cases=$results;noInputCaptureOrModeset=$true;limitations=@('Only owned diagnostic windows were inspected','No game window, rendering workload, or user input was observed','Physical DPI was not changed; scale-specific behavior remains bounded by the current host configuration')}|ConvertTo-Json -Depth 9|Set-Content -LiteralPath "$output\result.json"
    Write-Host "Desktop DPI evidence: $output"
    if(!$collected){throw "DPI evidence collection failed: $output"}
    if(!$passed -and !$AllowKnownRegression){throw "DPI geometry/awareness regression: $output"}
} finally {$env:WINEDLLOVERRIDES=$saved;$env:WINEDOWS_TRACE_DESKTOP=$savedTrace}
$global:LASTEXITCODE=0
