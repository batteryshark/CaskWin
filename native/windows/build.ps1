param([ValidateSet('x64','x86','all')][string]$Arch='all',[string]$OutputDirectory,[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
. (Join-Path $root 'wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$build=Join-Path $root 'build'
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
Initialize-CaskWinBuildOutput -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
New-Item -ItemType Directory -Force -Path $build,$OutputDirectory | Out-Null
$env:WINEDOWS_BIN=$OutputDirectory
# Compile Wine's original escaping routines, including their original copyright.
$wine=Get-Content -Raw -LiteralPath (Join-Path $WineSourceDirectory 'server\unicode.c')
foreach($marker in @('*/','/* number of following bytes','static unsigned short *casemap;','static inline char to_hex','static inline WCHAR to_lower','/* parse an escaped string','/* build a path')){
    if($wine.IndexOf($marker) -lt 0){throw "Selected Wine source has incompatible registry-codec extraction marker: $marker"}
}
$copyright=$wine.Substring(0,$wine.IndexOf('*/')+2)
$tables=$wine.Substring($wine.IndexOf('/* number of following bytes'),$wine.IndexOf('static unsigned short *casemap;')-$wine.IndexOf('/* number of following bytes'))
$hex=$wine.Substring($wine.IndexOf('static inline char to_hex'),$wine.IndexOf('static inline WCHAR to_lower')-$wine.IndexOf('static inline char to_hex'))
$funcs=$wine.Substring($wine.IndexOf('/* parse an escaped string'),$wine.IndexOf('/* build a path')-$wine.IndexOf('/* parse an escaped string'))
$generated=$copyright+"`n/* Extracted from Wine server/unicode.c. MSVC: ESC literal uses octal. */`n#include <windows.h>`n#include <stdio.h>`n#include <ctype.h>`ntypedef unsigned int data_size_t;`n"+$tables+$hex+$funcs.Replace("'\e'","'\033'")
[IO.File]::WriteAllText((Join-Path $build 'wine_unicode.c'),$generated)
& "$root\extract-version.ps1" -WineSourceDirectory $WineSourceDirectory -Output (Join-Path $build 'wine_version.inc')
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs) { throw 'Visual Studio C++ Build Tools are required for the Microsoft Detours dependency.' }
$env:WINEDOWS_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$env:TEMP=$build; $env:TMP=$build
$arches=if($Arch -eq 'all') {@('x64','x86')} else {@($Arch)}
foreach($a in $arches) {
    & $env:ComSpec /d /c "`"`"$root\build.cmd`" $a`""
    if($LASTEXITCODE) { throw "Native $a build failed with exit code $LASTEXITCODE" }
}
if($Arch -ne 'x86'){
    Copy-Item -LiteralPath (Join-Path $OutputDirectory 'wine64.exe') -Destination (Join-Path $OutputDirectory 'wine.exe') -Force
}
Write-CaskWinBuildSource -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
