param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\native-startup-fixtures'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    foreach($bits in @(32,64)){
        $toolchain=if($bits -eq 32){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        $env:PATH="$toolchain;$savedPath";$env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
        $compiler=Join-Path $toolchain 'g++.exe'
        if((& $compiler -dumpmachine) -ne $target){throw "Wrong compiler architecture: $bits"}
        $flags=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++')
        $nativeDef=Join-Path $OutputDirectory "native$bits.def"
        $builtinDef=Join-Path $OutputDirectory "builtin$bits.def"
        [IO.File]::WriteAllText($nativeDef,"LIBRARY wined3d.dll`nEXPORTS`nWinedowsNativeMarker @17`n")
        [IO.File]::WriteAllText($builtinDef,"LIBRARY wined3d.dll`nEXPORTS`nWinedowsBuiltinEntry @1`nWinedowsNativeMarker @17`n")
        $import=Join-Path $OutputDirectory "native-startup$bits.a"
        foreach($kind in @('private','app','builtin')){
            $value=switch($kind){'private'{'0x50524956'}'app'{'0x4150504c'}'builtin'{'0x4255494c'}}
            $definition=if($kind -eq 'builtin'){$builtinDef}else{$nativeDef}
            $dll=Join-Path $OutputDirectory "native-startup-$kind$bits.dll"
            $arguments=$flags+@('-shared',"-DWINEDOWS_MARKER_VALUE=$value","$PSScriptRoot\native-startup-marker.cpp",$definition,'-o',$dll)
            if($kind -eq 'private'){$arguments+="-Wl,--out-implib,$import"}
            & $compiler @arguments
            if($LASTEXITCODE){throw "Cannot build $kind $bits-bit startup marker"}
        }
        $probe=Join-Path $OutputDirectory "native-startup-probe$bits.exe"
        & $compiler @flags -municode "$PSScriptRoot\native-startup-probe.cpp" $import -o $probe
        if($LASTEXITCODE){throw "Cannot build $bits-bit static import probe"}
        $shadowDef=Join-Path $OutputDirectory "shadow$bits.def"
        $shadowImport=Join-Path $OutputDirectory "shadow$bits.a"
        [IO.File]::WriteAllText($shadowDef,"LIBRARY d3dcompiler_43.dll`nEXPORTS`nWinedowsNativeMarker @17`n")
        & (Join-Path $toolchain 'dlltool.exe') -d $shadowDef -D d3dcompiler_43.dll -l $shadowImport
        if($LASTEXITCODE){throw 'Cannot build host-shadow import library'}
        & $compiler @flags -municode -DWINEDOWS_SHADOW_IMPORT "$PSScriptRoot\native-startup-probe.cpp" $shadowImport -o (Join-Path $OutputDirectory "native-startup-shadow-probe$bits.exe")
        if($LASTEXITCODE){throw 'Cannot build host-shadow startup probe'}
        $bridgeDef=Join-Path $OutputDirectory "bridge$bits.def"
        $bridgeImport=Join-Path $OutputDirectory "bridge$bits.a"
        [IO.File]::WriteAllText($bridgeDef,"LIBRARY winedows-startup-bridge.dll`nEXPORTS`nWinedowsBridgeMarker @17`n")
        & $compiler @flags -shared "$PSScriptRoot\native-startup-bridge.cpp" $import $bridgeDef "-Wl,--out-implib,$bridgeImport" -o (Join-Path $OutputDirectory "native-startup-bridge$bits.dll")
        if($LASTEXITCODE){throw 'Cannot build transitive startup bridge'}
        & $compiler @flags -municode -DWINEDOWS_BRIDGE_IMPORT "$PSScriptRoot\native-startup-probe.cpp" $bridgeImport -o (Join-Path $OutputDirectory "native-startup-bridge-probe$bits.exe")
        if($LASTEXITCODE){throw 'Cannot build transitive startup probe'}
        $pe=(& (Join-Path $toolchain 'objdump.exe') -p $probe)-join "`n"
        if($LASTEXITCODE -or $pe -notmatch 'WinedowsNativeMarker' -or $pe -match 'DLL Name: (libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin)'){throw 'Invalid startup probe import table'}
        $exports=(& (Join-Path $toolchain 'objdump.exe') -p (Join-Path $OutputDirectory "native-startup-private$bits.dll"))-join "`n"
        if($LASTEXITCODE -or $exports -notmatch 'Ordinal Base\s+17'){throw 'Native marker unexpectedly provides ordinal 1'}
        Write-Host "Built named-import $bits-bit startup fixtures with native ordinal base 17"
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
