param(
    [ValidateSet('x86','x64','all')][string]$Arch='all',
    [int]$Jobs=8,
    [ValidateSet('winevulkan','vulkan-1')][string[]]$Modules=@('winevulkan','vulkan-1'),
    [string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'),
    [string]$WineSourceDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
. (Join-Path $PSScriptRoot 'wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$sourceIdentity=Get-CaskWinWineSourceIdentity -WineSourceDirectory $WineSourceDirectory
$bash='C:\msys64\usr\bin\bash.exe'
if(!(Test-Path -LiteralPath $bash)){throw 'Native Wine Vulkan builds require the existing MSYS2 MinGW build tools.'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
Initialize-CaskWinBuildOutput -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
$stage=Join-Path $PSScriptRoot 'build\native-vulkan'
$scripts=Join-Path $PSScriptRoot 'experimental\vulkan'
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
Push-Location $repo
try {
    & (Join-Path $scripts 'prepare.ps1') -StageDirectory $stage -WineSourceDirectory $WineSourceDirectory
    foreach($architecture in $arches){
        # Prepare only build tools/import libraries; never ship replacement NT
        # or kernel DLLs as a side effect of an explicit Vulkan-only build.
        & $bash 'native/windows/build-wine.sh' $architecture $Jobs '-' '-' 'prepare-vulkan' $WineSourceDirectory $sourceIdentity
        if($LASTEXITCODE){throw "Wine Vulkan prerequisites failed ($architecture)"}
        & (Join-Path $scripts 'build.ps1') -Arch $architecture -StageDirectory $stage -WineSourceDirectory $WineSourceDirectory
        if($LASTEXITCODE){throw "Wine Vulkan build failed ($architecture)"}
        $destination=Join-Path $OutputDirectory "builtin\$architecture"
        New-Item -ItemType Directory -Force -Path $destination|Out-Null
        Copy-Item -LiteralPath (Join-Path $stage "$architecture\winevulkan.dll") -Destination $destination
        if($Modules -contains 'vulkan-1'){
            & (Join-Path $scripts 'build-forwarder.ps1') -Arch $architecture -StageDirectory $stage -WineSourceDirectory $WineSourceDirectory
            if($LASTEXITCODE){throw "Wine Vulkan forwarder build failed ($architecture)"}
            Copy-Item -LiteralPath (Join-Path $stage "$architecture\vulkan-1.dll") -Destination $destination
        }
    }
} finally {
    Pop-Location
    $env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp
}
Write-CaskWinBuildSource -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
$global:LASTEXITCODE=0
