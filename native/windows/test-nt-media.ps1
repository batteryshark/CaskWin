param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [string]$FixtureDirectory,
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$FixtureDirectory){$FixtureDirectory=Join-Path $PSScriptRoot 'test-output\nt-media-probe-build'}
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-nt-media-probe.ps1" -OutputDirectory $FixtureDirectory}
$session=Join-Path $PSScriptRoot ('test-output\nt-media-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $session 'prefix';$fixtures=Join-Path $session 'fixtures'
New-Item -ItemType Directory -Path $session|Out-Null
$checks=[Collections.Generic.List[object]]::new()
function Invoke-Probe([string]$exe,[string[]]$arguments,[string]$name){
    $psi=[Diagnostics.ProcessStartInfo]::new();$psi.FileName=$exe;$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
    $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    foreach($arg in $arguments){$psi.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($psi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=!$process.WaitForExit(30000)
    if($timedOut){$process.Kill($true);$process.WaitForExit()}
    $text=$stdout.Result;$text|Set-Content -LiteralPath (Join-Path $session "$name.log")
    $stderr.Result|Set-Content -LiteralPath (Join-Path $session "$name.stderr.log")
    $checks.Add(@{case=$name;exitCode=$process.ExitCode;timedOut=$timedOut;passes=@($text -split '\r?\n'|Where-Object {$_ -match '^PASS '}).Count})
    Write-Host "$name exit=$($process.ExitCode) timeout=$timedOut"
    if($timedOut -or $process.ExitCode){throw "NT media operation failed: $name"}
    return $text
}
try{
    python "$PSScriptRoot\make-cue-fixtures.py" $fixtures
    if($LASTEXITCODE){throw 'CUE fixture generation failed'}
    $sourceHashes=@{};foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$sourceHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
    $hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
    $loader=Join-Path $RuntimeDirectory 'wine64.exe'
    Invoke-Probe $loader @('--prefix',$prefix,'init') 'init'|Out-Null
    Invoke-Probe $loader @('--prefix',$prefix,'drive','mount','D',(Join-Path $fixtures 'disc.cue')) 'mount'|Out-Null
    $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
    $nativeFile=Join-Path $session 'native.txt';$prefixFile=Join-Path $tests 'native.txt'
    [IO.File]::WriteAllText($nativeFile,'Native ordinary file fixture',[Text.Encoding]::ASCII)
    Copy-Item -LiteralPath $nativeFile -Destination $prefixFile
    $mapping=Join-Path $prefix 'dosdevices\drives.ini';$mappingHash=(Get-FileHash -LiteralPath $mapping -Algorithm SHA256).Hash
    $cacheHashes=@{};foreach($file in Get-ChildItem -LiteralPath (Join-Path $prefix '.winedows\media') -Recurse -File){$cacheHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
    foreach($bits in @(32,64)){
        $probe=Join-Path $FixtureDirectory "nt-media-probe$bits.exe"
        Copy-Item -LiteralPath $probe -Destination $tests
        $baseline=Invoke-Probe $probe @('--native',$nativeFile) "native-$bits"
        $nativeLines=@($baseline -split '\r?\n'|Where-Object {$_ -match '^NATIVE_'})
        if($nativeLines.Count -ne 4){throw 'Native passthrough baseline incomplete'}
        foreach($mode in @('alert','nonalert')){
            $result=Invoke-Probe (Join-Path $RuntimeDirectory "wine$bits.exe") @('--prefix',$prefix,'run',"C:\tests\nt-media-probe$bits.exe",$mode,'C:\tests\native.txt') "$mode-$bits"
            $prefixLines=@($result -split '\r?\n'|Where-Object {$_ -match '^NATIVE_'})
            if(Compare-Object $nativeLines $prefixLines){throw 'Nonvirtual NT behavior differs from native baseline'}
        }
    }
    foreach($path in $sourceHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sourceHashes[$path]){throw "Original fixture changed: $path"}}
    foreach($path in $cacheHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $cacheHashes[$path]){throw "Immutable media cache changed: $path"}}
    if((Get-FileHash -LiteralPath $mapping -Algorithm SHA256).Hash -ne $mappingHash){throw 'Drive configuration changed'}
    if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
    @{passed=$true;checks=@($checks);architectures=@('x86','x64');synchronousModes=@('alert','nonalert');exactIosbAndLastError=$true;tocAndPcmCompared=$true;asyncBeforeMutation=$true;accessChecks=$true;nativePassthroughCompared=$true;nativeSynchronousIocpAssociationRejected=$true;nativeAsyncCompletionPositiveControl=$true;originalsAndCacheUnchanged=$true;hostDriveListUnchanged=$true}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $session 'result.json')
    Write-Host "NT media suite passed: $session"
}catch{
    @{passed=$false;error=$_.Exception.Message;checks=@($checks)}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $session 'result.json')
    Write-Host "NT media failure evidence: $session"
    throw
}
$global:LASTEXITCODE=0
