param([Parameter(Mandatory=$true)][string]$RuntimeDirectory,
      [Parameter(Mandatory=$true)][string]$ProbeDirectory,
      [string]$OutputRoot=(Join-Path $PSScriptRoot 'test-output'),[switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot/build-legacy-registry-probe.ps1" -OutputDirectory $ProbeDirectory}
& python "$PSScriptRoot/test-legacy-registry.py" --runtime $RuntimeDirectory --probes $ProbeDirectory --output-root $OutputRoot
if($LASTEXITCODE){throw 'Legacy registry compatibility regression failed'}
