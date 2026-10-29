param([ValidateSet('x86','x64','all')][string]$Arch='all',[ValidateRange(1,128)][int]$Jobs=8,[ValidatePattern('^[a-z0-9_][a-z0-9_-]*$')][string[]]$Modules=@(),[string]$OutputDirectory,[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
. (Join-Path $PSScriptRoot 'wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$sourceIdentity=Get-CaskWinWineSourceIdentity -WineSourceDirectory $WineSourceDirectory
$bash='C:\msys64\usr\bin\bash.exe'
if(!(Test-Path -LiteralPath $bash)){throw 'Install MSYS2 and the mingw32/mingw64 GCC toolchains, make, flex, and bison.'}
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
Initialize-CaskWinBuildOutput -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
$vulkan=@($Modules|Where-Object {$_ -in @('winevulkan','vulkan-1')})
$ordinary=@($Modules|Where-Object {$_ -notin @('winevulkan','vulkan-1')})
if(!$Modules.Count){$vulkan=@('winevulkan','vulkan-1')}
Push-Location $repo
try {
    foreach($a in $arches){
        if(!$Modules.Count -or $ordinary.Count){
            & $bash 'native/windows/build-wine.sh' $a $Jobs ($ordinary -join ',') $OutputDirectory 'modules' $WineSourceDirectory $sourceIdentity
            if($LASTEXITCODE){throw "Wine $a build failed; see native/windows/build/wine-$a logs."}
        }
        if($vulkan.Count){
            $options=@{Arch=$a;Jobs=$Jobs;Modules=$vulkan;WineSourceDirectory=$WineSourceDirectory}
            if($OutputDirectory){$options.OutputDirectory=$OutputDirectory}
            & "$PSScriptRoot\build-vulkan.ps1" @options
            if($LASTEXITCODE){throw "Wine Vulkan $a build failed"}
        }
    }
} finally {Pop-Location}
Write-CaskWinBuildSource -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
$global:LASTEXITCODE=0
