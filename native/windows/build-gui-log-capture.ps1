param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\debug-09\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools required'}
foreach($bits in @(32,64)){
    $work=Join-Path $OutputDirectory "build$bits";New-Item -ItemType Directory -Force -Path $work|Out-Null;$arch=if($bits -eq 32){'x64_x86'}else{'x64'};$builder=Join-Path $work 'build.cmd'
    $lines=@('@echo off',('call "{0}\VC\Auxiliary\Build\vcvarsall.bat" {1} >nul' -f $vs,$arch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $work),('set "TEMP={0}"' -f $work),('set "TMP={0}"' -f $work),('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE "{0}\gui-log-capture.cpp" /Fe:"{1}\gui-log-capture{2}.exe"' -f $PSScriptRoot,$OutputDirectory,$bits),'if errorlevel 1 exit /b 1',('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE "{0}\gui-log-child.cpp" /Fe:"{1}\gui-log-child{2}.exe" /link /SUBSYSTEM:WINDOWS shell32.lib' -f $PSScriptRoot,$OutputDirectory,$bits),'exit /b %errorlevel%')
    [IO.File]::WriteAllLines($builder,$lines);& $env:ComSpec /d /c $builder;if($LASTEXITCODE){throw "GUI log fixture build failed: $bits"}
}
$global:LASTEXITCODE=0
