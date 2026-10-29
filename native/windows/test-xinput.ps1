param([switch]$SkipBuild,[string]$RuntimeDirectory)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-xinput-probe.ps1" -OutputDirectory $RuntimeDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\xinput-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
$versions=@('1_1','1_2','1_3','1_4','9_1_0');$names=@($versions|ForEach-Object{"xinput$_"})
$failure=[Collections.Generic.List[string]]::new()
$records=[Collections.Generic.List[object]]::new()
function Invoke-XInput([string]$label,[string]$program,[string[]]$arguments,[switch]$HostNative,[string]$Loader=$runtime,[switch]$ExpectedFailure) {
    if($HostNative){$output=& $program @arguments 2>&1}
    else{$output=& $Loader --prefix $prefix @arguments 2>&1}
    $exit=$LASTEXITCODE
    $output|Tee-Object -FilePath (Join-Path $suite "$label.log")|Write-Host
    $passed=if($ExpectedFailure){$exit -ne 0 -and ($output -join "`n") -match 'No available implementation for xinput1_4\.dll'}else{$exit -eq 0}
    if(!$passed){$failure.Add("$label (exit $exit)")}
    $availability=@($output|Select-String '^RESULT connected=(\d+) failures=(\d+)$'|ForEach-Object{[int]$_.Matches[0].Groups[1].Value})
    $records.Add(@{case=$label;exit=$exit;passed=$passed;connectedControllers=$availability})
}
$saved=$env:WINEDLLOVERRIDES
try {
    $env:WINEDLLOVERRIDES=$null
    foreach($bits in @(32,64)){
        foreach($version in $versions){
            Invoke-XInput "host-$version-$bits" (Join-Path $RuntimeDirectory "xinput-probe$bits.exe") @("xinput$version.dll",'--native') -HostNative
        }
    }
    if($failure.Count){throw 'Native XInput baseline failed; inspect suite logs before trusting injected results.'}
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'XInput prefix initialization failed'}
    $directory=Join-Path $prefix 'drive_c\tests'
    New-Item -ItemType Directory -Force -Path $directory|Out-Null
    foreach($bits in @(32,64)){
        Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "xinput-probe$bits.exe") -Destination $directory
        foreach($version in @('1_3','1_4','9_1_0')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "xinput-import-$version-$bits.exe") -Destination $directory}
    }
    foreach($order in @('b,n','n,b')){
        $env:WINEDLLOVERRIDES=($names -join ',')+"=$order"
        foreach($bits in @(32,64)){
            foreach($version in $versions){
                $arguments=@('run',"C:\tests\xinput-probe$bits.exe","xinput$version.dll")
                if($order -eq 'n,b'){$arguments+='--native'}
                Invoke-XInput "dynamic-$($order.Replace(',',''))-$version-$bits" '' $arguments
            }
            foreach($version in @('1_3','1_4','9_1_0')){
                $arguments=@('run',"C:\tests\xinput-import-$version-$bits.exe","xinput$version.dll")
                if($order -eq 'n,b'){$arguments+='--native'}
                Invoke-XInput "startup-$($order.Replace(',',''))-$version-$bits" '' $arguments
            }
        }
    }
    $env:WINEDLLOVERRIDES=($names -join ',')+'=b'
    foreach($bits in @(32,64)){
        Invoke-XInput "desktop-$bits" '' @('--desktop','XInput,640x480','run',"C:\tests\xinput-import-1_4-$bits.exe",'xinput1_4.dll')
    }
    foreach($system in @('system32','syswow64')){
        $shadow=Join-Path $prefix "drive_c\windows\$system"
        New-Item -ItemType Directory -Force -Path $shadow|Out-Null
        'Deliberately invalid private DLL: hardware transport must use the trusted host system module.'|Set-Content -LiteralPath (Join-Path $shadow 'xinput1_4.dll')
    }
    foreach($bits in @(32,64)){
        Invoke-XInput "private-system-shadow-$bits" '' @('run',"C:\tests\xinput-probe$bits.exe",'xinput1_3.dll')
    }
    foreach($system in @('system32','syswow64')){
        Remove-Item -LiteralPath (Join-Path $prefix "drive_c\windows\$system\xinput1_4.dll")
    }
    # A separate runtime copy proves b,n fallback without renaming shared DLLs.
    $minimal=Join-Path $suite 'runtime-without-builtins'
    New-Item -ItemType Directory -Path $minimal|Out-Null
    foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $minimal}
    foreach($bits in @(32,64)){
        $env:WINEDLLOVERRIDES='xinput1_4=b,n'
        Invoke-XInput "missing-builtin-fallback-$bits" '' @('run',"C:\tests\xinput-import-1_4-$bits.exe",'xinput1_4.dll','--native') -Loader (Join-Path $minimal 'wine64.exe')
        $env:WINEDLLOVERRIDES='xinput1_4=b'
        Invoke-XInput "strict-missing-builtin-$bits" '' @('run',"C:\tests\xinput-probe$bits.exe",'xinput1_4.dll') -Loader (Join-Path $minimal 'wine64.exe') -ExpectedFailure
    }
}finally{
    $env:WINEDLLOVERRIDES=$saved
    @{
        passed=($failure.Count -eq 0 -and $records.Count -ge 50);runtime=$RuntimeDirectory;architectures=@('x86','x64');
        variants=$versions;cases=@($records);failures=@($failure);
        coverage=@('native and Wine dynamic providers','startup import binding','export and ordinal contracts','no-device queries','trusted Windows hardware backend identity and private system-DLL shadow resistance','virtual desktop','missing builtin fallback and strict rejection');
        limitations=@('Physical button/axis transitions, hotplug, vibration and controller audio are unverified','Generic HID-to-XInput mapping provided by Wine bus on Linux is not implemented by this Windows transport');
        inputInjected=$false;vibrationRequested=$false;controllerContentsLogged=$false;utc=[DateTime]::UtcNow.ToString('o')
    }|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "XInput results: $suite"
}
if($failure.Count){throw "XInput cases failed: $($failure -join ', ')"}
$global:LASTEXITCODE=0
