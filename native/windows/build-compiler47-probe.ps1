param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\compiler47-fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC Build Tools and Windows SDK are required for the D3D11 compiler probe.'}
$env:COMPILER47_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat';$env:COMPILER47_SOURCE=$PSScriptRoot;$env:COMPILER47_OUTPUT=$OutputDirectory
foreach($bits in @(32,64)){
    $work=Join-Path $OutputDirectory "build$bits";New-Item -ItemType Directory -Force -Path $work|Out-Null
    $env:COMPILER47_WORK=$work;$target=if($bits -eq 32){'x64_x86'}else{'x64'}
    $builder=Join-Path $work 'build.cmd'
@'
@echo off
call "%COMPILER47_VCVARS%" %1 >nul
if errorlevel 1 exit /b 1
cd /d "%COMPILER47_WORK%"
set "TEMP=%COMPILER47_WORK%"
set "TMP=%COMPILER47_WORK%"
set "FLAGS=/nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00"
cl %FLAGS% "%COMPILER47_SOURCE%\compiler47-probe.cpp" /Fe:"%COMPILER47_OUTPUT%\compiler47-probe%2.exe" /link user32.lib dxguid.lib
if errorlevel 1 exit /b 1
cl %FLAGS% /DWINEDOWS_STATIC_COMPILER47 "%COMPILER47_SOURCE%\compiler47-probe.cpp" /Fe:"%COMPILER47_OUTPUT%\compiler47-import-probe%2.exe" /link user32.lib dxguid.lib d3dcompiler.lib
'@|Set-Content -LiteralPath $builder
    & $env:ComSpec /d /c $builder $target $bits
    if($LASTEXITCODE){throw "Cannot build $bits-bit compiler47 fixtures"}
}
$global:LASTEXITCODE=0
