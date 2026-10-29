param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\display-api-09\fixtures'),
    [switch]$SkipBuild,
    [switch]$ExpectOldFailure
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-display-api-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\display-api-09\run-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$prefix=Join-Path $suite 'prefix';$loader=Join-Path $RuntimeDirectory 'wine64.exe'
$cases=[Collections.Generic.List[object]]::new()
$saved=@{overrides=$env:WINEDLLOVERRIDES;config=$env:WINE_D3D_CONFIG;trace=$env:WINEDOWS_TRACE}
function Run-DisplayCase([string]$name,[string]$exe,[string[]]$arguments,[bool]$expectedFailure=$false){
    $start=[Diagnostics.ProcessStartInfo]::new($exe);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in $arguments){$start.ArgumentList.Add($arg)}
    $p=[Diagnostics.Process]::Start($start);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
    if(!$p.WaitForExit(20000)){$p.Kill($true);$p.WaitForExit();$code=124}else{$code=$p.ExitCode}
    $content=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$p.Dispose()
    [IO.File]::WriteAllText((Join-Path $suite "$name.log"),$content)
    $count=([regex]::Matches($content,'(?m)^PASS (simple|extended)-')).Count
    $passed=if($expectedFailure){$code -ne 0 -and $content -match 'FAULT case=simple-A-input0-guard'}else{$code -eq 0 -and $count -eq 20 -and $content -match 'PASS display API buffer contract cases=20'}
    $cases.Add(@{name=$name;passed=$passed;exitCode=$code;expectedFailure=$expectedFailure;bufferCases=$count});Write-Host "$name passed=$passed exit=$code cases=$count"
    if(!$passed){Write-Host $content;throw "Display API contract failed: $name"}
    return $content
}
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINE_D3D_CONFIG=$null;$env:WINEDOWS_TRACE=$null
    & $loader --prefix $prefix init *> (Join-Path $suite 'init.log');if($LASTEXITCODE){throw 'Display API prefix init failed'}
    $programs=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $programs|Out-Null
    foreach($bits in @(32,64)){
        $fixture=Join-Path $FixtureDirectory "display-api-probe$bits.exe";Copy-Item -LiteralPath $fixture -Destination $programs
        foreach($mode in @('current','registry','first')){
            $env:WINEDLLOVERRIDES=$null
            $before=Run-DisplayCase "host-before-$bits-$mode" $fixture @('--mode',$mode)
            if($before -notmatch 'RESULT case=simple-A-input0-guard size=124 extra=0 width=(\d+) height=(\d+)'){throw 'Native display dimensions missing'}
            $width=$Matches[1];$height=$Matches[2]
            if($mode -eq 'current'){$initialWidth=$width;$initialHeight=$height}
            $env:WINEDLLOVERRIDES='ddraw,d3d9=n'
            $null=Run-DisplayCase "prefix-native-$bits-$mode" $loader @('--prefix',$prefix,'--desktop','off','run',"C:\tests\display-api-probe$bits.exe",'--mode',$mode,'--expect-size',$width,$height)
            $env:WINEDLLOVERRIDES='ddraw,d3d9=b'
            # The existing Wine display registry view is the session's initial
            # current mode, so resetting a guest cannot restore an old host mode.
            $physicalWidth=if($mode -eq 'registry'){$initialWidth}else{$width};$physicalHeight=if($mode -eq 'registry'){$initialHeight}else{$height}
            $null=Run-DisplayCase "prefix-physical-$bits-$mode" $loader @('--prefix',$prefix,'--desktop','off','run',"C:\tests\display-api-probe$bits.exe",'--mode',$mode,'--expect-size',$physicalWidth,$physicalHeight) ([bool]$ExpectOldFailure)
            $virtualWidth=if($mode -eq 'first'){'320'}else{'640'};$virtualHeight=if($mode -eq 'first'){'240'}else{'480'}
            $null=Run-DisplayCase "prefix-virtual-$bits-$mode" $loader @('--prefix',$prefix,'--desktop','Display API,640x480','run',"C:\tests\display-api-probe$bits.exe",'--mode',$mode,'--expect-size',$virtualWidth,$virtualHeight) ([bool]$ExpectOldFailure)
            $env:WINEDLLOVERRIDES=$null
            $null=Run-DisplayCase "host-after-$bits-$mode" $fixture @('--mode',$mode,'--expect-size',$width,$height)
        }
    }
    @{passed=$true;runtime=$RuntimeDirectory;cases=$cases;expectedOldFailure=[bool]$ExpectOldFailure;scope='Read-only display queries, native and prefix physical/virtual API contract, exact guard pages plus32-byte canaries, no display mode changes, GPU device creation, audio playback or UI input.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Display API results: $suite"
}catch{@{passed=$false;runtime=$RuntimeDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw}
finally{$env:WINEDLLOVERRIDES=$saved.overrides;$env:WINE_D3D_CONFIG=$saved.config;$env:WINEDOWS_TRACE=$saved.trace}
$global:LASTEXITCODE=0
