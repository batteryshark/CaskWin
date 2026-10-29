param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\reader-queue-08\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools are required'}
foreach($bits in @(32,64)){
    $work=Join-Path $OutputDirectory "build$bits";New-Item -ItemType Directory -Force -Path $work|Out-Null
    $arch=if($bits -eq 32){'x64_x86'}else{'x64'};$library=if($bits -eq 32){'X86'}else{'X64'};$builder=Join-Path $work 'build.cmd'
    $lines=@('@echo off',('call "{0}\VC\Auxiliary\Build\vcvarsall.bat" {1} >nul' -f $vs,$arch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $work),('set "TEMP={0}"' -f $work),('set "TMP={0}"' -f $work),('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "{0}\reader-queue-probe.cpp" /Fe:"{1}\reader-queue-probe{2}.exe" /link "{0}\vendor\Detours-4.0.1\lib.{3}\detours.lib" mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib' -f $PSScriptRoot,$OutputDirectory,$bits,$library),'exit /b %errorlevel%')
    [IO.File]::WriteAllLines($builder,$lines);& $env:ComSpec /d /c $builder;if($LASTEXITCODE){throw "Reader queue fixture build failed: $bits"}
}
$global:LASTEXITCODE=0
