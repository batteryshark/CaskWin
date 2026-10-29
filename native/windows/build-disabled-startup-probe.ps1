param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\disabled-startup-fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ tools required for native startup validation fixture'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    $env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
    foreach($bits in @(32,64)){
        $vcarch=if($bits -eq 32){'x64_x86'}else{'x64'}
        $script=Join-Path $OutputDirectory "build-validator$bits.cmd"
        $lines=@('@echo off','setlocal',('call "{0}" {1} >nul' -f $vcvars,$vcarch),'if errorlevel 1 exit /b 1',('cd /d "{0}"' -f $OutputDirectory),('cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS "{0}\disabled-startup-probe.cpp" "{0}\startup-imports.cpp" /Fe:"{1}\disabled-startup-probe{2}.exe"' -f $PSScriptRoot,$OutputDirectory,$bits),'exit /b %errorlevel%')
        [IO.File]::WriteAllLines($script,$lines);& $env:ComSpec /d /c $script
        if($LASTEXITCODE){throw "Disabled startup validator build failed ($bits)"}
        $tool="C:\msys64\mingw$bits\bin";$env:PATH="$tool;$savedPath"
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-static','-static-libgcc','-static-libstdc++')
        foreach($kind in @('leaf','bridge','forward','ordinal','contract','unknown-contract')){
            $def=Join-Path $OutputDirectory "$kind$bits.def";$out=Join-Path $OutputDirectory "wd-disable-$kind$bits.dll"
            $name=if($kind -eq 'leaf'){'leaf'}else{'bridge'}
            $export=switch($kind){'leaf'{'ProbeValue @17'}'bridge'{'BridgeValue @23'}'forward'{'BridgeValue=wd-disable-leaf.ProbeValue @23'}'ordinal'{'BridgeValue="wd-disable-leaf.#17" @23'}'contract'{'BridgeValue=api-ms-win-core-processthreads-l1-1-0.GetCurrentProcessId @23'}'unknown-contract'{'BridgeValue=api-ms-win-winedows-nonexistent-l1-1-0.ProbeValue @23'}}
            [IO.File]::WriteAllText($def,"LIBRARY wd-disable-$name.dll`nEXPORTS`n$export`n")
            $define=if($kind -notin @('leaf','bridge')){'-DDISABLED_FORWARD'}else{"-DDISABLED_$($kind.ToUpperInvariant())"}
            $args=$flags+@('-shared',$define,"$PSScriptRoot\disabled-startup-fixture.cpp",$def,'-o',$out)
            if($kind -eq 'bridge'){$args+=Join-Path $OutputDirectory "leaf$bits.a"}
            & "$tool\g++.exe" @args;if($LASTEXITCODE){throw "Disabled $kind DLL fixture failed ($bits)"}
            if($kind -in @('leaf','bridge')){& "$tool\dlltool.exe" -d $def -D "wd-disable-$name.dll" -l (Join-Path $OutputDirectory "$kind$bits.a");if($LASTEXITCODE){throw 'Fixture import library failed'}}
        }
        foreach($kind in @('leaf','bridge','empty')){
            $args=$flags+@("$PSScriptRoot\disabled-startup-fixture.cpp",'-o',(Join-Path $OutputDirectory "disabled-startup-$kind$bits.exe"))
            if($kind -ne 'empty'){$args+=Join-Path $OutputDirectory "$kind$bits.a"}else{$args+='-DDISABLED_EMPTY_CHILD'}
            if($kind -eq 'bridge'){$args+='-DDISABLED_BRIDGE_CHILD'}
            & "$tool\g++.exe" @args;if($LASTEXITCODE){throw "Disabled startup child build failed ($bits)"}
        }
        foreach($prefix in @('api','ext')){
            $def=Join-Path $OutputDirectory "$prefix$bits.def";$import=Join-Path $OutputDirectory "$prefix$bits.a"
            [IO.File]::WriteAllText($def,"LIBRARY $prefix-helper.dll`nEXPORTS`nBridgeValue @23`n")
            & "$tool\dlltool.exe" -d $def -D "$prefix-helper.dll" -l $import
            if($LASTEXITCODE){throw 'Ordinary API-prefixed fixture import failed'}
            $args=$flags+@('-DDISABLED_BRIDGE_CHILD',"$PSScriptRoot\disabled-startup-fixture.cpp",$import,'-o',(Join-Path $OutputDirectory "disabled-startup-$prefix$bits.exe"))
            & "$tool\g++.exe" @args;if($LASTEXITCODE){throw 'Ordinary API-prefixed fixture child failed'}
        }
    }
    & "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:anycpu /out:"$OutputDirectory\disabled-startup-managed.exe" "$PSScriptRoot\disabled-startup-managed.cs"
    if($LASTEXITCODE){throw 'AnyCPU startup fixture compilation failed'}
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
