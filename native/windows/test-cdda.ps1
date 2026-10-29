param([switch]$SkipBuild,[string]$RuntimeDirectory)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory;& "$PSScriptRoot\build-wine.ps1" -OutputDirectory $RuntimeDirectory;& "$PSScriptRoot\build-cdda-probe.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\cdda-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'CDDA prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests'
$fixtures=Join-Path $prefix '.winedows\media\fixtures'
New-Item -ItemType Directory -Path $directory|Out-Null
& python "$PSScriptRoot\make-cdda-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'CDDA fixture generation failed'}
& $runtime --prefix $prefix drive audio D (Join-Path $fixtures 'valid\disc.ini')
if($LASTEXITCODE){throw 'Silent mixed-mode CDDA fixture mount failed'}
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$PSScriptRoot\bin\cdda-probe$bits.exe" -Destination $directory}
$saved=$env:WINEDLLOVERRIDES
$checks=[System.Collections.Generic.List[object]]::new()
function Quote-CddaArgument([string]$value){
    $escaped=[regex]::Replace($value,'(\\*)"','$1$1\"')
    $escaped=[regex]::Replace($escaped,'(\\+)$','$1$1')
    return '"'+$escaped+'"'
}
function Invoke-CddaFixture([string]$name,[string[]]$arguments){
    $stdout=Join-Path $prefix "$name.log";$stderr=Join-Path $prefix "$name-stderr.log"
    $commandLine=(@('--prefix',$prefix)+$arguments|ForEach-Object{Quote-CddaArgument $_})-join ' '
    $process=Start-Process -FilePath $runtime -ArgumentList $commandLine -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        if(!$process.WaitForExit(90000)){
            # The loader owns a kill-on-close job containing only this fixture.
            $process.Kill();$process.WaitForExit();throw "CDDA fixture timed out: $name"
        }
        $code=$process.ExitCode
        Get-Content -LiteralPath $stdout
        if((Get-Item -LiteralPath $stderr).Length){Get-Content -LiteralPath $stderr}
        $checks.Add(@{fixture=$name;exitCode=$code;status=$(if($code){'failed'}else{'passed'})})
        if($code){throw "CDDA fixture failed: $name (exit $code)"}
    } finally {$process.Dispose()}
}
try {
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $driver="$RuntimeDirectory\builtin\$arch\mcicda.dll"
        foreach($mode in @('b','n')){
            $env:WINEDLLOVERRIDES="mcicda=b;dsound=$mode"
            $sound=if($mode -eq 'b'){"$RuntimeDirectory\builtin\$arch\dsound.dll"}else{'native'}
            Invoke-CddaFixture "mci-$bits-$mode" @('run',"C:\tests\cdda-probe$bits.exe",$driver,$sound)
        }
        $env:WINEDLLOVERRIDES='mcicda,dsound=b'
        Invoke-CddaFixture "desktop-$bits" @('--desktop','CDDA,800x600','run',"C:\tests\cdda-probe$bits.exe",$driver,"$RuntimeDirectory\builtin\$arch\dsound.dll")
    }
    @{passed=$true;checks=$checks;silentTrackPlayed=2;capturePerformed=$false;limitations=@('Inherited Wine track-type strings are numeric 1088/1089, not localized audio/other','Subchannel position represents the reader/seek cursor, not an exact playback clock','Natural-completion MCI_NOTIFY and MCI_WAIT playback semantics retain upstream Wine limitations');utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "Wine CDDA MCI suite passed: $prefix"
} catch {
    @{passed=$false;checks=$checks;error=$_.Exception.Message;silentTrackPlayed=2;capturePerformed=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    throw
} finally {$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
