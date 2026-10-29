param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\context-fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory);New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools are required for the DLL manifest fixture'}
foreach($bits in @(32,64)){
    $temporary=Join-Path $OutputDirectory "build$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $arch=if($bits -eq 32){'x64_x86'}else{'x64'};$processor=if($bits -eq 32){'x86'}else{'amd64'};$manifest=Join-Path $temporary 'A.manifest'
    [IO.File]::WriteAllText($manifest,"<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`"><assemblyIdentity type=`"win32`" name=`"Winedows.Builtin.Parent`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/><dependency><dependentAssembly><assemblyIdentity type=`"win32`" name=`"Winedows.Builtin.Boundary`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/></dependentAssembly></dependency></assembly>")
    $source=Join-Path $PSScriptRoot 'builtin-context-probe.cpp';$build=Join-Path $temporary 'build.cmd'
    $flags='/nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE'
    $lines=@('@echo off',('call "{0}\VC\Auxiliary\Build\vcvarsall.bat" {1} >nul' -f $vs,$arch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $temporary),
      ('cl {0} /LD /DWINEDOWS_CONTEXT_B "{1}" /Fe:B.dll' -f $flags,$source),'if errorlevel 1 exit /b 1',
      ('copy /y B.dll "{0}\context-real{1}.dll" >nul' -f $OutputDirectory,$bits),
      ('cl {0} /LD /DWINEDOWS_CONTEXT_A "{1}" /Fe:A.dll /link B.lib /MANIFEST:EMBED,ID=2 /MANIFESTUAC:NO /MANIFESTINPUT:"{2}"' -f $flags,$source,$manifest),'if errorlevel 1 exit /b 1',
      ('copy /y A.dll "{0}\context-parent{1}.dll" >nul' -f $OutputDirectory,$bits),
      ('cl {0} "{1}" /Fe:"{2}\context-probe{3}.exe" /link A.lib' -f $flags,$source,$OutputDirectory,$bits),'if errorlevel 1 exit /b 1',
      ('cl {0} /LD /DWINEDOWS_CONTEXT_SHADOW "{1}" /Fe:B.dll /link gdiplus.lib' -f $flags,$source),'if errorlevel 1 exit /b 1',
      ('copy /y B.dll "{0}\context-shadow{1}.dll" >nul' -f $OutputDirectory,$bits),'exit /b 0')
    [IO.File]::WriteAllLines($build,$lines);& $env:ComSpec /d /c $build;if($LASTEXITCODE){throw 'DLL-owned activation context fixture compilation failed'}
}
$global:LASTEXITCODE=0
