param([string]$RuntimeDirectory,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\dpi-08\fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-dpi-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\dpi-08\gui-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$checks=[Collections.Generic.List[object]]::new()
function Invoke-Owned([string]$Program,[string[]]$Arguments){
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$Program;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    foreach($arg in $Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);if(!$process.WaitForExit(15000)){$process.Kill($true);$process.WaitForExit();throw 'Owned GUI fixture timed out'}
    $code=$process.ExitCode;$process.Dispose();if($code){throw "Owned GUI fixture exit $code"}
}
$saved=$env:WINEDOWS_TRACE_DESKTOP;$savedOverrides=$env:WINEDLLOVERRIDES
try {
    $env:WINEDOWS_TRACE_DESKTOP=$null;$env:WINEDLLOVERRIDES=$null
    & $runtime --prefix $prefix init|Out-Host;if($LASTEXITCODE){throw 'GUI prefix init failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){
        $file="desktop-dpi-gui$bits.exe";Copy-Item -LiteralPath "$FixtureDirectory\$file" -Destination "$directory\$file"
        $baseline=Join-Path $output "host-$bits.json";Invoke-Owned "$FixtureDirectory\$file" @($baseline);$native=Get-Content -LiteralPath $baseline -Raw|ConvertFrom-Json
        foreach($enabled in @($false,$true)){
            $env:WINEDOWS_TRACE_DESKTOP=if($enabled){'1'}else{$null};$label="gui-$bits-trace$enabled";$resultFile="$directory\$label.json"
            Invoke-Owned $runtime @('--prefix',$prefix,'--desktop','GuiDpi,800x600','run',"C:\tests\$file","C:\tests\$label.json")
            $result=Get-Content -LiteralPath $resultFile -Raw|ConvertFrom-Json;$traceFile=Join-Path $prefix ".winedows\desktop-$($result.pid).log"
            $exists=Test-Path -LiteralPath $traceFile;$lines=if($exists){@(Get-Content -LiteralPath $traceFile)}else{@()}
            $sameErrors=$result.invalidError -eq $native.invalidError
            # SetWindowPos documents GetLastError only on FALSE. Successful
            # calls may retain or change it; record that observation separately.
            # https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowpos
            $sameSuccessError=$result.validError -eq $native.validError
            $traceCorrect=if($enabled){$exists -and $lines.Count -eq 128 -and !($lines|Where-Object {$_ -notlike 'WINEDOWS_DESKTOP *'})}else{!$exists}
            $passed=$result.passed -and $native.passed -and $sameErrors -and $traceCorrect
            $checks.Add(@{name=$label;passed=$passed;native=$native;result=$result;failureLastErrorMatchesNative=$sameErrors;successLastErrorMatchesNativeInformational=$sameSuccessError;traceExists=$exists;traceRecords=$lines.Count;traceFile=$traceFile})
            Write-Host "$label passed=$passed traceRecords=$($lines.Count)"
        }
    }
    $passed=!($checks|Where-Object {!$_.passed});@{passed=$passed;runtime=$RuntimeDirectory;cases=$checks;noInputCaptureOrModeset=$true}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$output\result.json"
    Write-Host "GUI trace evidence: $output";if(!$passed){throw 'GUI desktop trace regression'}
}finally{$env:WINEDOWS_TRACE_DESKTOP=$saved;$env:WINEDLLOVERRIDES=$savedOverrides}
$global:LASTEXITCODE=0
