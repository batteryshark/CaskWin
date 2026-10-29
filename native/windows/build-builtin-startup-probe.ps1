param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools are required for Detours fixtures'}
$detours=Join-Path $PSScriptRoot 'vendor\Detours-4.0.1';$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{foreach($bits in @(32,64)){
    $temporary=Join-Path $OutputDirectory "build$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $env:TEMP=$temporary;$env:TMP=$temporary;$arch=if($bits -eq 32){'x64_x86'}else{'x64'};$library=if($bits -eq 32){'X86'}else{'X64'}
    $def=Join-Path $temporary 'helper.def';[IO.File]::WriteAllText($def,"EXPORTS`r`n DetourFinishHelperProcess @1`r`n")
    $build=Join-Path $temporary 'build.cmd'
    $lines=@('@echo off',('call "{0}\VC\Auxiliary\Build\vcvarsall.bat" {1} >nul' -f $vs,$arch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $temporary),
      ('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /I"{0}\include" /LD "{1}\startup-imports-helper.cpp" /Fe:"{2}\startup-imports-helper{3}.dll" /link /DEF:"{4}" "{0}\lib.{5}\detours.lib"' -f $detours,$PSScriptRoot,$OutputDirectory,$bits,$def,$library),'if errorlevel 1 exit /b 1',
      ('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /I"{0}\include" "{1}\builtin-binding-probe.cpp" "{1}\startup-imports.cpp" /Fe:"{2}\builtin-binding-probe{3}.exe" /link "{0}\lib.{4}\detours.lib"' -f $detours,$PSScriptRoot,$OutputDirectory,$bits,$library),'exit /b %errorlevel%')
    [IO.File]::WriteAllLines($build,$lines);& $env:ComSpec /d /c $build
    if($LASTEXITCODE){throw 'Cannot build selected builtin binding fixture'}
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
    $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& "$toolchain\g++.exe" -dumpmachine) -ne $target){throw 'Wrong builtin fixture architecture'}
    $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++')
    & "$toolchain\g++.exe" @flags -municode "$PSScriptRoot\builtin-child-probe.cpp" -o "$OutputDirectory\builtin-child-probe$bits.exe"
    if($LASTEXITCODE){throw 'Cannot build selected builtin child fixture'}
    & "$toolchain\g++.exe" @flags -municode "$PSScriptRoot\builtin-bound-probe.cpp" -o "$OutputDirectory\builtin-bound-probe$bits.exe" -lgdiplus -lversion
    if($LASTEXITCODE){throw 'Cannot build native bound-import fixture'}
    & "$toolchain\g++.exe" @flags -shared -DWINEDOWS_TRANSITIVE_DLL "$PSScriptRoot\builtin-transitive-probe.cpp" -o "$temporary\builtin-transitive.dll" "-Wl,--out-implib,$temporary\transitive.a" -lgdiplus
    if($LASTEXITCODE){throw 'Cannot build transitive builtin dependency'}
    Copy-Item -LiteralPath "$temporary\builtin-transitive.dll" -Destination "$OutputDirectory\builtin-transitive$bits.dll"
    & "$toolchain\g++.exe" @flags -municode "$PSScriptRoot\builtin-transitive-probe.cpp" "$temporary\transitive.a" -o "$OutputDirectory\builtin-transitive-probe$bits.exe"
    if($LASTEXITCODE){throw 'Cannot build transitive builtin fixture'}
}}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
