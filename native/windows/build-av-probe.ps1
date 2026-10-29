param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-05'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC Build Tools required'}
$env:AV_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat';$env:AV_SOURCE=$PSScriptRoot;$env:AV_OUTPUT=$OutputDirectory
foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
    $work=Join-Path $PSScriptRoot "build\av-probe-$a";New-Item -ItemType Directory -Force -Path $work|Out-Null;$env:AV_WORK=$work
    $bits=if($a -eq 'x86'){32}else{64};$target=if($a -eq 'x86'){'x64_x86'}else{'x64'};$detours=if($a -eq 'x86'){'X86'}else{'X64'}
    $builder=Join-Path $work 'build.cmd'
@'
@echo off
call "%AV_VCVARS%" %1 >nul
if errorlevel 1 exit /b 1
cd /d "%AV_WORK%"
set "TEMP=%AV_WORK%"
set "TMP=%AV_WORK%"
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "%AV_SOURCE%\av-probe.cpp" /Fe:"%AV_OUTPUT%\av-probe%2.exe" /link "%AV_SOURCE%\vendor\Detours-4.0.1\lib.%3\detours.lib" d3d11.lib dxgi.lib dxguid.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "%AV_SOURCE%\video-defaults-probe.cpp" "%AV_SOURCE%\video-backend.cpp" "%AV_SOURCE%\registry-views.cpp" /Fe:"%AV_OUTPUT%\video-defaults-probe%2.exe" /link advapi32.lib
'@|Set-Content -LiteralPath $builder
    & $env:ComSpec /d /c $builder $target $bits $detours
    if($LASTEXITCODE){throw "Audiovisual fixture build failed: $a"}
}
$global:LASTEXITCODE=0
