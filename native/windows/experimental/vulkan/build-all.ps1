param([string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop';$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
. (Join-Path $PSScriptRoot '..\..\wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
& (Join-Path $PSScriptRoot 'prepare.ps1') -StageDirectory $StageDirectory -WineSourceDirectory $WineSourceDirectory
foreach($arch in @('x86','x64')){
 & (Join-Path $PSScriptRoot 'build.ps1') -Arch $arch -StageDirectory $StageDirectory -WineSourceDirectory $WineSourceDirectory
 & (Join-Path $PSScriptRoot 'build-forwarder.ps1') -Arch $arch -StageDirectory $StageDirectory -WineSourceDirectory $WineSourceDirectory
}
& (Join-Path $PSScriptRoot 'build-probe.ps1') -StageDirectory $StageDirectory -WineSourceDirectory $WineSourceDirectory
Write-Host "Experimental Wine Vulkan artifacts: $StageDirectory"
$global:LASTEXITCODE=0
