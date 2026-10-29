param(
    [Parameter(Mandatory=$true)][string]$FixtureDirectory,
    [ValidateRange(5,30)][int]$Seconds=10
)
$ErrorActionPreference='Stop'
$output=Join-Path $PSScriptRoot ('test-output\game-audio-waveout-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output|Out-Null
$checks=[Collections.Generic.List[object]]::new()
foreach($rate in @(44100,48000)){
    foreach($buffers in @(2,4)){
        $name="native-$rate-$buffers-buffers"
        $psi=[Diagnostics.ProcessStartInfo]::new()
        $psi.FileName=[IO.Path]::GetFullPath((Join-Path $FixtureDirectory 'game-audio-waveout-probe64.exe'))
        $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
        $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
        foreach($arg in @("$buffers","$rate","$Seconds",(Join-Path $output "$name.json"))){$psi.ArgumentList.Add($arg)}
        $process=[Diagnostics.Process]::Start($psi)
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $timedOut=!$process.WaitForExit(($Seconds+15)*1000)
        if($timedOut){$process.Kill($true);$process.WaitForExit()}
        $stdout.Result|Set-Content -LiteralPath (Join-Path $output "$name.log")
        $stderr.Result|Set-Content -LiteralPath (Join-Path $output "$name.stderr.log")
        if($timedOut -or $process.ExitCode){throw "Native WinMM control unavailable: $name exit=$($process.ExitCode) timeout=$timedOut"}
        $result=Get-Content -Raw -LiteralPath (Join-Path $output "$name.json")|ConvertFrom-Json
        $checks.Add($result);Write-Host $stdout.Result.Trim()
    }
}
@{allCollectionsSucceeded=$true;checks=@($checks);limitations=@('Native WinMM completion/position metadata only; hardware underruns are not measured','Only this fixture queue depth changes; host audio configuration is untouched')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'result.json')
Write-Host "Native WinMM queue evidence: $output"
$global:LASTEXITCODE=0
