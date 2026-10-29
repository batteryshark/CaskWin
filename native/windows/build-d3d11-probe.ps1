param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC Build Tools required'}
$env:D3D11_PROBE_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat';$env:D3D11_PROBE_SOURCE=$PSScriptRoot;$env:D3D11_PROBE_OUTPUT=$OutputDirectory
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
foreach($a in $arches){
    $work=Join-Path $PSScriptRoot "build\d3d11-probe-$a";New-Item -ItemType Directory -Force -Path $work|Out-Null
    $env:D3D11_PROBE_WORK=$work;$bits=if($a -eq 'x86'){32}else{64};$target=if($a -eq 'x86'){'x64_x86'}else{'x64'}
    $builder=Join-Path $work 'build.cmd'
@'
@echo off
call "%D3D11_PROBE_VCVARS%" %1 >nul
if errorlevel 1 exit /b 1
cd /d "%D3D11_PROBE_WORK%"
set "TEMP=%D3D11_PROBE_WORK%"
set "TMP=%D3D11_PROBE_WORK%"
set "FLAGS=/nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00"
cl %FLAGS% "%D3D11_PROBE_SOURCE%\d3d11-probe.cpp" /Fe:"%D3D11_PROBE_OUTPUT%\d3d11-probe%2.exe" /link user32.lib gdi32.lib opengl32.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /DWINEDOWS_STATIC_D3D11_IMPORT "%D3D11_PROBE_SOURCE%\d3d11-probe.cpp" /Fe:"%D3D11_PROBE_OUTPUT%\d3d11-import-probe%2.exe" /link user32.lib gdi32.lib opengl32.lib dxguid.lib d3d11.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%D3D11_PROBE_SOURCE%\d3d10-probe.cpp" /Fe:"%D3D11_PROBE_OUTPUT%\d3d10-probe%2.exe" /link user32.lib dxguid.lib
'@ | Set-Content -LiteralPath $builder
    & $env:ComSpec /d /c $builder $target $bits
    if($LASTEXITCODE){throw "D3D11 $a probe build failed"}
}
$global:LASTEXITCODE=0
