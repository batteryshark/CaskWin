param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-readerfix'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\debug-09\fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-gui-log-capture.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\debug-09\capture-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new();$prefix=Join-Path $suite 'prefix';$files=@{}
function Invoke-Capture([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Log,[int]$ExpectedExit=0,[bool]$Flood=$false,[bool]$WineTrace=$false,[bool]$LaunchFailure=$false){
    $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true;$start.Environment['WINEDEBUG']='+gdiplus';$start.Environment.Remove('WINEDLLOVERRIDES')|Out-Null
    foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText("$suite\$Name-wrapper.log",$text)
    $report=Get-Content -LiteralPath ($Log+'.status.json') -Raw|ConvertFrom-Json;$size=(Get-Item -LiteralPath $Log).Length
    $pass=$code -eq $ExpectedExit -and !$report.running -and !$report.drainIncomplete -and !$report.readError -and !$report.writeError -and $report.savedBytes -eq $size -and $report.limitBytes -eq 16MB
    if($LaunchFailure){$pass=$pass -and $report.launchError -ne 0 -and !$report.childPid -and !$size}
    else{
        $pass=$pass -and $report.childPid -gt 0 -and $report.childExitCode -eq $ExpectedExit -and !$report.launchError
        $file=[IO.File]::OpenRead($Log);try{$buffer=[byte[]]::new(4096);$count=$file.Read($buffer,0,$buffer.Length);$head=[Text.Encoding]::UTF8.GetString($buffer,0,$count)}finally{$file.Dispose()}
        $pass=$pass -and $head -match 'GUI_STDOUT_MARKER' -and $head -match 'GUI_STDERR_CRT_MARKER' -and $head -match 'GUI_STDERR_WRITEFILE_MARKER' -and $head -match 'stdout=3 stderr=3 rawWrite=1'
        if($Flood){$pass=$pass -and $size -eq 16MB -and $report.truncated -and $report.discardedBytes -gt 1MB -and $report.receivedBytes -eq $report.savedBytes+$report.discardedBytes}
        else{$pass=$pass -and !$report.truncated -and !$report.discardedBytes;if($WineTrace){$all=[IO.File]::ReadAllText($Log);$pass=$pass -and $all -match '(trace|warn|fixme|err):gdiplus:' -and $all -match 'GUI_WINE_GDIPLUS status=0 token=1'}else{$pass=$pass -and $head -match 'GUI_ARGUMENT_QUOTING_OK'}}
    }
    $cases.Add(@{name=$Name;passed=[bool]$pass;exitCode=$code;status=$report});Write-Host "$Name passed=$pass exit=$code bytes=$size discarded=$($report.discardedBytes)"
    if(!$pass){Write-Host $text;Write-Host ($report|ConvertTo-Json);throw "GUI capture case failed: $Name"}
}
try{
    foreach($bits in @(32,64)){foreach($name in @('gui-log-capture','gui-log-child')){$file="$FixtureDirectory\$name$bits.exe";$files[$file]=(Get-FileHash -LiteralPath $file).Hash}}
    foreach($parent in @(32,64)){foreach($child in @(32,64)){
        $log="$suite\native-$parent-to-$child.log";Invoke-Capture "native-$parent-to-$child" "$FixtureDirectory\gui-log-capture$parent.exe" @($log,"$FixtureDirectory\gui-log-child$child.exe",'--arg','space and quote " plus trailing\') $log
    }}
    foreach($bits in @(32,64)){
        $log="$suite\native-exit-$bits.log";Invoke-Capture "native-exit-$bits" "$FixtureDirectory\gui-log-capture$bits.exe" @($log,"$FixtureDirectory\gui-log-child$bits.exe",'--exit','37','--arg','space and quote " plus trailing\') $log 37
        $log="$suite\native-cap-$bits.log";Invoke-Capture "native-cap-$bits" "$FixtureDirectory\gui-log-capture$bits.exe" @($log,"$FixtureDirectory\gui-log-child$bits.exe",'--flood') $log 0 $true
        $log="$suite\missing-child-$bits.log";Invoke-Capture "missing-child-$bits" "$FixtureDirectory\gui-log-capture$bits.exe" @($log,"$suite\absent-child$bits.exe") $log 1 $false $false $true
    }
    & "$RuntimeDirectory\wine64.exe" --prefix $prefix init;if($LASTEXITCODE){throw 'Prefix init failed'}
    $inside=Join-Path $prefix 'drive_c\capture';New-Item -ItemType Directory -Path $inside|Out-Null
    foreach($bits in @(32,64)){foreach($name in @('gui-log-capture','gui-log-child')){Copy-Item -LiteralPath "$FixtureDirectory\$name$bits.exe" -Destination $inside}}
    foreach($parent in @(32,64)){foreach($child in @(32,64)){
        $log="$inside\wine-$parent-to-$child.log";$arch=if($child -eq 32){'x86'}else{'x64'};$provider="$RuntimeDirectory\builtin\$arch\gdiplus.dll";$files[$provider]=(Get-FileHash -LiteralPath $provider).Hash
        Invoke-Capture "wine-$parent-to-$child" "$RuntimeDirectory\wine64.exe" @('--prefix',$prefix,'run',"C:\capture\gui-log-capture$parent.exe","C:\capture\wine-$parent-to-$child.log","C:\capture\gui-log-child$child.exe",'--wine-gdiplus',$provider) $log 0 $false $true
    }}
    foreach($bits in @(32,64)){
        $log="$inside\wine-cap-$bits.log";Invoke-Capture "wine-cap-$bits" "$RuntimeDirectory\wine64.exe" @('--prefix',$prefix,'run',"C:\capture\gui-log-capture$bits.exe","C:\capture\wine-cap-$bits.log","C:\capture\gui-log-child$bits.exe",'--flood') $log 0 $true
    }
    foreach($file in $files.Keys){if((Get-FileHash -LiteralPath $file).Hash -ne $files[$file]){throw "Fixture/provider changed: $file"}}
    @{passed=$true;runtime=$RuntimeDirectory;cases=$cases;sourceFilesUnchanged=$true;scope='GUI-subsystem child with no windows/input/GPU; native and injected cross-architecture stdout/stderr, genuine Wine gdiplus trace, exit/error preservation and continuous drain beyond16MiB cap.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$suite\result.json";Write-Host "GUI capture results: $suite"
}catch{@{passed=$false;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$suite\result.json";throw}
$global:LASTEXITCODE=0
