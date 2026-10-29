param([switch]$SkipBuild,[string]$RuntimeDirectory,[string]$FixtureDirectory)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
if(!$FixtureDirectory){$FixtureDirectory=Join-Path $PSScriptRoot 'bin'}
if(!$SkipBuild){& "$PSScriptRoot\build-com-marshaling-probe.ps1" -OutputDirectory $FixtureDirectory}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$suite=Join-Path $PSScriptRoot ('test-output\com-marshaling-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$checks=[Collections.Generic.List[object]]::new()
function Host-Snapshot {
    $rows=[Collections.Generic.List[object]]::new()
    $paths=@('Software\Classes\Interface\{0000000C-0000-0000-C000-000000000046}','Software\Classes\Interface\{0000000C-0000-0000-C000-000000000046}\ProxyStubClsid32','Software\Classes\Interface\{0C733A30-2A1C-11CE-ADE5-00AA0044773D}','Software\Classes\Interface\{0C733A30-2A1C-11CE-ADE5-00AA0044773D}\ProxyStubClsid32','Software\Classes\CLSID\{00000320-0000-0000-C000-000000000046}','Software\Classes\CLSID\{00000320-0000-0000-C000-000000000046}\InprocServer32')
    foreach($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)) {
        foreach($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser)) {
            $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
            try {foreach($path in $paths){
                $key=$root.OpenSubKey($path)
                try {$rows.Add([ordered]@{hive=$hive.ToString();view=$view.ToString();path=$path;exists=($null -ne $key);value=$(if($key){$key.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)});threading=$(if($key){$key.GetValue('ThreadingModel')})})}finally{if($key){$key.Dispose()}}
            }}finally{$root.Dispose()}
        }
    }
    return ConvertTo-Json -InputObject @($rows) -Depth 6 -Compress
}
function Initialize-Prefix([string]$path,[int]$bits) {
    & $runtime --prefix $path init | Out-Null
    if($LASTEXITCODE){throw "Cannot initialize COM fixture prefix: $path"}
    $tests=Join-Path $path 'drive_c\tests';New-Item -ItemType Directory -Force -Path $tests|Out-Null
    Copy-Item -LiteralPath (Join-Path $FixtureDirectory "com-marshaling-probe$bits.exe") -Destination (Join-Path $tests 'marshal.exe')
}
function Invoke-Case([string]$label,[int]$bits,[string]$prefix,[string]$mode,[string]$apartment='sta') {
    $stdout=Join-Path $suite "$label.log";$stderr=Join-Path $suite "$label.stderr.log"
    if($mode -eq '--host'){$exe=Join-Path $FixtureDirectory "com-marshaling-probe$bits.exe";$args=@($mode,$apartment)}
    else{$exe=$runtime;$args=@('--prefix',('"'+$prefix+'"'),'run','C:\tests\marshal.exe',$mode,$apartment)}
    $process=Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timeout=!$process.WaitForExit(30000)
    if($timeout){& taskkill.exe /PID $process.Id /T /F 2>&1|Out-Null;$process.WaitForExit()}
    $process.Refresh();$exit=$process.ExitCode
    $checks.Add([ordered]@{case=$label;exitCode=$exit;timedOut=$timeout})
    Get-Content -LiteralPath $stdout
    if($timeout -or $exit -ne 0){throw "COM marshaling case failed: $label (exit $exit)"}
}
$before=Host-Snapshot
$before|Set-Content -LiteralPath (Join-Path $suite 'host-before.json')
try {
    foreach($bits in @(32,64)) {
        Invoke-Case "host-sta-$bits" $bits '' '--host' 'sta'
        Invoke-Case "host-mta-$bits" $bits '' '--host' 'mta'
        $primary=Join-Path $suite "primary$bits";$sibling=Join-Path $suite "sibling$bits"
        Initialize-Prefix $primary $bits;Initialize-Prefix $sibling $bits
        Invoke-Case "prefix-sta-$bits" $bits $primary '--exercise' 'sta'
        Invoke-Case "prefix-mta-$bits" $bits $primary '--exercise' 'mta'
        Invoke-Case "seed-machine-$bits" $bits $primary '--machine-block'
        Invoke-Case "respect-machine-$bits" $bits $primary '--expect-block'
        Invoke-Case "seed-user-$bits" $bits $primary '--user-restore'
        Invoke-Case "respect-user-sta-$bits" $bits $primary '--verify-overrides' 'sta'
        Invoke-Case "respect-user-mta-$bits" $bits $primary '--verify-overrides' 'mta'
        Invoke-Case "sibling-sta-$bits" $bits $sibling '--exercise' 'sta'
        Invoke-Case "sibling-mta-$bits" $bits $sibling '--exercise' 'mta'
        $empty=Join-Path $suite "empty$bits";Initialize-Prefix $empty $bits
        Invoke-Case "seed-empty-$bits" $bits $empty '--empty-sequential'
        Invoke-Case "respect-empty-$bits" $bits $empty '--expect-empty'
        $class=Join-Path $suite "class$bits";Initialize-Prefix $class $bits
        Invoke-Case "seed-class-$bits" $bits $class '--class-block'
        Invoke-Case "respect-class-data-$bits" $bits $class '--expect-class-preserved'
    }
    $after=Host-Snapshot;$after|Set-Content -LiteralPath (Join-Path $suite 'host-after.json')
    if($before -cne $after){throw 'Host standard COM registrations changed'}
    @{passed=$true;checks=@($checks);hostUnchanged=$true;coverage=@('real non-agile IStream STA to MTA and MTA to STA','binary read/write and sequential interface queries','GIT cross-apartment stream transfer','private machine and user interface priority across launcher restarts','existing empty and broken registration preservation','sibling isolation','unrelated host CLSID and Interface remain hidden','no PSFactoryBuffer class registration imported');limitations=@('only IStream/ISequentialStream interface defaults','native COM provides PSFactoryBuffer intrinsically and does not use its private class registration to select the factory','cross-process COM and custom proxy deployments not covered')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "COM marshaling suite passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;checks=@($checks)}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw
}
$global:LASTEXITCODE=0
