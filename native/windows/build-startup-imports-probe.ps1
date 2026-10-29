param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\startup-imports-fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& "$PSScriptRoot\build-native-startup-probe.ps1" -OutputDirectory $OutputDirectory
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools are required by the existing Detours library'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$detours=Join-Path $PSScriptRoot 'vendor\Detours-4.0.1'
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    $env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x64_x86'}else{'x64'};$library=if($bits -eq 32){'X86'}else{'X64'}
        $def=Join-Path $OutputDirectory "helper$bits.def"
        [IO.File]::WriteAllText($def,"EXPORTS`r`n    DetourFinishHelperProcess @1`r`n")
        $build=Join-Path $OutputDirectory "build-msvc$bits.cmd"
        $lines=@('@echo off','setlocal',('call "{0}" {1} >nul' -f $vcvars,$arch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $OutputDirectory),('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"{0}\include" /LD "{1}\startup-imports-helper.cpp" /Fe:"{2}\startup-imports-helper{3}.dll" /link /DEF:"{4}" "{0}\lib.{5}\detours.lib"' -f $detours,$PSScriptRoot,$OutputDirectory,$bits,$def,$library),'if errorlevel 1 exit /b 1',('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"{0}\include" "{1}\startup-imports-probe.cpp" "{1}\startup-imports.cpp" /Fe:"{2}\startup-imports-probe{3}.exe" /link "{0}\lib.{4}\detours.lib"' -f $detours,$PSScriptRoot,$OutputDirectory,$bits,$library),'exit /b %errorlevel%')
        $processor=if($bits -eq 32){'x86'}else{'amd64'}
        $manifest=Join-Path $OutputDirectory "sxs$bits.manifest"
        [IO.File]::WriteAllText($manifest,"<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`"><assemblyIdentity type=`"win32`" name=`"Winedows.Startup.Application`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/><dependency><dependentAssembly><assemblyIdentity type=`"win32`" name=`"Winedows.Startup.SideBySide`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/></dependentAssembly></dependency></assembly>")
        $lines=$lines[0..($lines.Count-2)]+@('if errorlevel 1 exit /b 1')
        foreach($kind in @('direct','bridge')){
            $define=if($kind -eq 'bridge'){'/DWINEDOWS_BRIDGE_IMPORT'}else{'/DWINEDOWS_DIRECT_IMPORT'}
            $import=if($kind -eq 'bridge'){"bridge$bits.a"}else{"native-startup$bits.a"}
            $lines+=('cl /nologo /std:c++17 /EHsc /MT /O2 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS {0} "{1}\startup-imports-child.cpp" /Fe:"{2}\startup-imports-sxs-{3}{4}.exe" /link "{2}\{5}" /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:"{6}"' -f $define,$PSScriptRoot,$OutputDirectory,$kind,$bits,$import,$manifest)
            $lines+='if errorlevel 1 exit /b 1'
        }
        $lines+=('cl /nologo /std:c++17 /EHsc /MT /O2 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DWINEDOWS_STARTUP_IAT_IDENTITY "{0}\native-startup-probe.cpp" /Fe:"{1}\startup-runtime-sxs{2}.exe" /link "{1}\native-startup{2}.a" /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:"{3}"' -f $PSScriptRoot,$OutputDirectory,$bits,$manifest)
        $lines+='if errorlevel 1 exit /b 1'
        $lines+='exit /b 0'
        [IO.File]::WriteAllLines($build,$lines)
        & $env:ComSpec /d /c $build
        if($LASTEXITCODE){throw "Cannot build isolated Detours $bits-bit startup fixture"}
        $tool=if($bits -eq 32){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'};$env:PATH="$tool;$savedPath"
        $flags=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-municode')
        foreach($kind in @('direct','bridge','shadow')){
            $define=switch($kind){'bridge'{'-DWINEDOWS_BRIDGE_IMPORT'}'shadow'{'-DWINEDOWS_SHADOW_IMPORT'}default{'-DWINEDOWS_DIRECT_IMPORT'}}
            $import=switch($kind){'bridge'{"bridge$bits.a"}'shadow'{"shadow$bits.a"}default{"native-startup$bits.a"}}
            & "$tool\g++.exe" @flags $define "$PSScriptRoot\startup-imports-child.cpp" (Join-Path $OutputDirectory $import) -o (Join-Path $OutputDirectory "startup-imports-$kind$bits.exe")
            if($LASTEXITCODE){throw "Cannot build $kind $bits-bit child"}
        }
        foreach($suffix in @(32,64)){
            $definition=Join-Path $OutputDirectory "rename$suffix-$bits.def";$import=Join-Path $OutputDirectory "rename$suffix-$bits.a"
            [IO.File]::WriteAllText($definition,"LIBRARY wd-provider$suffix.dll`nEXPORTS`nWinedowsNativeMarker @17`n")
            & "$tool\dlltool.exe" -d $definition -D "wd-provider$suffix.dll" -l $import
            if($LASTEXITCODE){throw 'Cannot build filename substitution fixture library'}
            & "$tool\g++.exe" @flags "$PSScriptRoot\startup-imports-child.cpp" $import -o (Join-Path $OutputDirectory "startup-imports-rename$suffix-$bits.exe")
            if($LASTEXITCODE){throw 'Cannot build filename substitution fixture'}
        }
        $dllFlags=@('-std=c++17','-O2','-static','-static-libgcc','-static-libstdc++','-shared')
        & "$tool\g++.exe" @dllFlags "$PSScriptRoot\startup-imports-cycle.cpp" (Join-Path $OutputDirectory "bridge$bits.a") (Join-Path $OutputDirectory "native$bits.def") -o (Join-Path $OutputDirectory "startup-imports-cycle$bits.dll")
        if($LASTEXITCODE){throw 'Cannot build native dependency cycle fixture'}
        $forwardDef=Join-Path $OutputDirectory "forward$bits.def"
        [IO.File]::WriteAllText($forwardDef,"LIBRARY wined3d.dll`nEXPORTS`nWinedowsNativeMarker=wd-startup-target.WinedowsNativeMarker @17`n")
        & "$tool\g++.exe" @dllFlags "$PSScriptRoot\startup-imports-forward.cpp" $forwardDef -o (Join-Path $OutputDirectory "startup-imports-forward$bits.dll")
        if($LASTEXITCODE){throw 'Cannot build native export-forwarding fixture'}
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
