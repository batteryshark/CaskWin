param([Parameter(Mandatory=$true)][string]$RuntimeDirectory,
      [Parameter(Mandatory=$true)][string]$ProbeDirectory,
      [string]$OutputRoot=(Join-Path $PSScriptRoot 'test-output'),[switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot/build-legacy-launch-probe.ps1" -OutputDirectory $ProbeDirectory}
& python "$PSScriptRoot/test-legacy-launch.py" --runtime $RuntimeDirectory --probes $ProbeDirectory --output-root $OutputRoot
if($LASTEXITCODE){throw 'Legacy launch regression failed'}
