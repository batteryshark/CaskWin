param([string]$RuntimeDirectory,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\dpi-08\callback-fixtures'),[switch]$SkipBuild,[switch]$AllowKnownRegression)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-callback-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\dpi-08\callbacks-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$cases=[Collections.Generic.List[object]]::new()
function Invoke-Case([string]$Name,[string]$Program,[string[]]$Arguments){
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Program;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in $Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timeout=!$process.WaitForExit(15000);if($timeout){$process.Kill($true);$process.WaitForExit()}
    $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$process.Dispose()
    $text|Set-Content -LiteralPath "$output\$Name.log";$errors|Set-Content -LiteralPath "$output\$Name.stderr.log"
    $events=@($text -split "`r?`n"|Where-Object {$_ -like '{*'}|ForEach-Object {$_|ConvertFrom-Json});$summary=$events|Where-Object {$_.kind -eq 'summary'}|Select-Object -Last 1
    $cases.Add(@{name=$Name;exitCode=$code;timedOut=$timeout;summary=$summary;events=$events})
    Write-Host "$Name exit=$code callbacks=$($summary.observations) mismatches=$($summary.mismatches)"
}
$saved=$env:WINEDLLOVERRIDES;$savedTrace=$env:WINEDOWS_TRACE_DESKTOP
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_TRACE_DESKTOP=$null
    & $runtime --prefix $prefix init|Out-Host;if($LASTEXITCODE){throw 'Callback prefix init failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){
        $file="desktop-callback$bits.exe";Copy-Item -LiteralPath "$FixtureDirectory\$file" -Destination "$directory\$file"
        foreach($encoding in @('unicode','ansi')){foreach($creation in @('hidden','visible')){
            Invoke-Case "host-$bits-$encoding-$creation" "$FixtureDirectory\$file" @($encoding,$creation)
            Invoke-Case "prefix-$bits-$encoding-$creation" $runtime @('--prefix',$prefix,'--desktop','Callbacks,800x600','run',"C:\tests\$file",$encoding,$creation)
        }}
    }
    $collected=!($cases|Where-Object {$_.timedOut -or !$_.summary});$passed=$collected -and !($cases|Where-Object {$_.exitCode})
    @{passed=$passed;collected=$collected;runtime=$RuntimeDirectory;cases=$cases;scope='Ordinary and nested callback queries, mutations, coordinate continuity and native RTL roundtrips';limitations=@('A tail jump retaining a USER32 return address keeps native fallback; recorded separately','Mixed hidden/contained HWND mapping stays physical and need not compose through exposed screen coordinates','Only already-loaded genuine system graphics backends are retained in the native caller cache')}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath "$output\result.json"
    Write-Host "Callback evidence: $output";if(!$collected -or (!$passed -and !$AllowKnownRegression)){throw 'Desktop callback regression'}
}finally{$env:WINEDLLOVERRIDES=$saved;$env:WINEDOWS_TRACE_DESKTOP=$savedTrace}
$global:LASTEXITCODE=0
