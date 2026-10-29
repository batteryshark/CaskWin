param([ValidateSet('x86','x64','all')][string]$Arch='all')
$ErrorActionPreference='Stop'
$savedPath=$env:PATH; $savedTemp=$env:TEMP; $savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
try {
    foreach($a in $arches){
        $toolchain=if($a -eq 'x86'){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $env:PATH="$toolchain;$savedPath"
        $temp=Join-Path $PSScriptRoot "build\d3d8-probe-$a"
        New-Item -ItemType Directory -Force -Path $temp,(Join-Path $PSScriptRoot 'bin')|Out-Null
        $env:TEMP=$temp; $env:TMP=$temp
        $compiler=Join-Path $toolchain 'g++.exe'
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $expected){throw "Wrong $a compiler target"}
        $importLibrary='-ld3d8'
        if($a -eq 'x64'){
            # Windows has no x64 d3d8 provider/import library. Wine does: build
            # only the import descriptor needed to exercise its startup binding.
            $definition=Join-Path $temp 'd3d8.def'
            [IO.File]::WriteAllText($definition,"LIBRARY d3d8.dll`nEXPORTS`nDirect3DCreate8`n")
            $importLibrary=Join-Path $temp 'libd3d8-probe.a'
            & (Join-Path $toolchain 'dlltool.exe') -d $definition -l $importLibrary -D d3d8.dll
            if($LASTEXITCODE){throw 'Cannot create the x64 D3D8 probe import library'}
        }
        $bits=if($a -eq 'x86'){32}else{64}
        foreach($static in @($false,$true)){
            $name=if($static){"d3d8-import-probe$bits.exe"}else{"d3d8-probe$bits.exe"}
            $output=Join-Path "$PSScriptRoot\bin" $name
            $args=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-D_WIN32_WINNT=0x0601','-DUNICODE','-D_UNICODE','-static','-static-libgcc','-static-libstdc++','-municode',"$PSScriptRoot\d3d8-probe.cpp",'-o',$output,'-luser32','-lgdi32','-lopengl32')
            if($static){$args+=@('-DWINEDOWS_STATIC_D3D8_IMPORT',$importLibrary)}
            & $compiler @args
            if($LASTEXITCODE){throw "D3D8 probe build failed: $name"}
            $imports=& (Join-Path $toolchain 'objdump.exe') -p $output | Select-String 'DLL Name:'
            if($LASTEXITCODE){throw "Cannot inspect imports: $name"}
            if($imports -match 'libgcc_s|libstdc\+\+|libwinpthread'){throw "Unexpected dynamic MinGW runtime dependency: $name"}
            Write-Host "Built $name ($expected; static C/C++ runtimes)"
        }
    }
} finally {$env:PATH=$savedPath; $env:TEMP=$savedTemp; $env:TMP=$savedTmp}
$global:LASTEXITCODE=0
