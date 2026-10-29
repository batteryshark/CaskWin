param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC Build Tools required'}
$env:WSI_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat';$env:WSI_SOURCE=$PSScriptRoot;$env:WSI_OUTPUT=$OutputDirectory
foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
 $work=Join-Path $PSScriptRoot "build\wined3d-vulkan-$a";New-Item -ItemType Directory -Force -Path $work|Out-Null;$env:WSI_WORK=$work
 $bits=if($a -eq 'x86'){32}else{64};$target=if($a -eq 'x86'){'x64_x86'}else{'x64'};$detours=if($a -eq 'x86'){'X86'}else{'X64'};$builder=Join-Path $work 'build.cmd'
@'
@echo off
call "%WSI_VCVARS%" %1 >nul
if errorlevel 1 exit /b 1
cd /d "%WSI_WORK%"
set "TEMP=%WSI_WORK%"
set "TMP=%WSI_WORK%"
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "%WSI_SOURCE%\wined3d-vulkan-probe.cpp" /Fe:"%WSI_OUTPUT%\wined3d-vulkan-probe%2.exe" /link "%WSI_SOURCE%\vendor\Detours-4.0.1\lib.%3\detours.lib" user32.lib dxguid.lib
'@|Set-Content -LiteralPath $builder
 & $env:ComSpec /d /c $builder $target $bits $detours
 if($LASTEXITCODE){throw "WineD3D Vulkan fixture build failed: $a"}
}
$global:LASTEXITCODE=0
