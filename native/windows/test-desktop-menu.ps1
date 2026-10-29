param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime10-desktop'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\display-10\fixtures'),[switch]$SkipBuild,[switch]$NativeOnly)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-menu-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\display-10\menu-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output|Out-Null
$results=[Collections.Generic.List[object]]::new()
function Invoke-Bounded([string]$Name,[string]$Program,[string[]]$Arguments){
    $log=Join-Path $output "$Name.log";$errorLog=Join-Path $output "$Name.stderr.log"
    # No console window, but do not set STARTF_USESHOWWINDOW/SW_HIDE: that
    # suppresses the actual desktop host's first ShowWindow and invalidates
    # paint/visibility checks. All visible windows are owned fixture windows.
    $start=[Diagnostics.ProcessStartInfo]::new($Program)
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    if(!$process.Start()){throw "Could not start owned fixture: $Name"}
    $outputRead=$process.StandardOutput.ReadToEndAsync();$errorRead=$process.StandardError.ReadToEndAsync()
    try {
        $completed=$process.WaitForExit(25000)
        if(!$completed){$process.Kill($true);[void]$process.WaitForExit(2000)}
        $code=if($completed){$process.ExitCode}else{124}
        if($outputRead.Wait(2000)){$outputRead.Result|Set-Content -LiteralPath $log}else{'Output pipe did not close within deadline'|Set-Content -LiteralPath $log;$code=124}
        if($errorRead.Wait(2000)){$errorRead.Result|Set-Content -LiteralPath $errorLog}else{'Error pipe did not close within deadline'|Set-Content -LiteralPath $errorLog;$code=124}
        $results.Add(@{name=$Name;exitCode=$code;timedOut=(!$completed);passed=($completed -and $code -eq 0);log=$log;stderr=$errorLog})
        Write-Host "$Name exit=$code"
        if($code -eq 124){throw "Owned fixture exceeded its deadline: $Name; remaining cases stopped"}
        if($code -ne 0){throw "Owned fixture failed: $Name; remaining cases stopped"}
    } finally {$process.Dispose()}
}
$savedOverrides=$env:WINEDLLOVERRIDES
try {
    $env:WINEDLLOVERRIDES=$null
    foreach($bits in @(32,64)){foreach($encoding in @('ansi','unicode')){Invoke-Bounded "native-$bits-$encoding" "$FixtureDirectory\desktop-menu-probe$bits.exe" @('native',$encoding)}}
    if(!$NativeOnly){
        $prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
        Invoke-Bounded 'prefix-init' $runtime @('--prefix',$prefix,'init')
        if(!$results[$results.Count-1].passed){throw 'Prefix initialization failed'}
        $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
        foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\desktop-menu-probe$bits.exe" -Destination "$tests\menu$bits.exe"}
        foreach($bits in @(32,64)){foreach($child in @(32,64)){foreach($encoding in @('ansi','unicode')){
            Invoke-Bounded "virtual-$bits-to-$child-$encoding" $runtime @('--prefix',$prefix,'--desktop','Menu,800x600','run',"C:\tests\menu$bits.exe",'virtual',$encoding,"C:\tests\menu$child.exe")
        }}}
    }
} finally {
    $env:WINEDLLOVERRIDES=$savedOverrides
    $passed=$results.Count -gt 0 -and !($results|Where-Object {!$_.passed})
    @{passed=$passed;runtime=$RuntimeDirectory;cases=$results;noGlobalInput=$true;scope='Hidden unowned menu roots and post-adoption logical owners; visible-at-create, owner promotion during hidden creation, and late-added menus are separate gaps.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$output\result.json"
    Write-Host "Desktop menu results: $output"
}
if(!$passed){throw "Desktop menu regression failed: $output"}
$global:LASTEXITCODE=0
