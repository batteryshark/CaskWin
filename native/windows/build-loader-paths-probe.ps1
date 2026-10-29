param([ValidateSet('x86','x64','all')][string]$Arch='all')
$ErrorActionPreference='Stop'
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    $arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
    foreach($a in $arches){
        $toolchain=if($a -eq 'x86'){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $env:PATH="$toolchain;$savedPath"
        $temp=Join-Path $PSScriptRoot "build\loader-paths-$a"
        New-Item -ItemType Directory -Force -Path $temp,(Join-Path $PSScriptRoot 'bin')|Out-Null
        $env:TEMP=$temp;$env:TMP=$temp
        $compiler=Join-Path $toolchain 'g++.exe'
        $target=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $target){throw "Wrong compiler architecture: $a"}
        $bits=if($a -eq 'x86'){32}else{64}
        $flags=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-D_WIN32_WINNT=0x0A00','-DUNICODE','-D_UNICODE','-static','-static-libgcc','-static-libstdc++')
        $resource=Join-Path $temp 'marker.o'
        & (Join-Path $toolchain 'windres.exe') "$PSScriptRoot\loader-paths-marker.rc" -O coff -o $resource
        if($LASTEXITCODE){throw 'Cannot build loader marker resource'}
        $marker=Join-Path $PSScriptRoot "bin\loader-paths-marker$bits.dll"
        & $compiler @flags -shared "$PSScriptRoot\loader-paths-marker.cpp" $resource -o $marker
        if($LASTEXITCODE){throw 'Cannot build loader marker DLL'}
        $d3d8='-ld3d8'
        if($a -eq 'x64'){
            $definition=Join-Path $temp 'd3d8.def';[IO.File]::WriteAllText($definition,"LIBRARY d3d8.dll`nEXPORTS`nDirect3DCreate8`n")
            $d3d8=Join-Path $temp 'libd3d8-probe.a'
            & (Join-Path $toolchain 'dlltool.exe') -d $definition -l $d3d8 -D d3d8.dll
            if($LASTEXITCODE){throw 'Cannot build x64 D3D8 import descriptor'}
        }
        $outputs=@($marker)
        foreach($static in @($false,$true)){
            $name=if($static){"loader-paths-import-probe$bits.exe"}else{"loader-paths-probe$bits.exe"}
            $output=Join-Path $PSScriptRoot "bin\$name"
            $args=$flags+@('-municode',"$PSScriptRoot\loader-paths-probe.cpp",'-o',$output)
            if($static){$args+=@('-DWINEDOWS_STATIC_LOADER_IMPORTS','-lddraw',$d3d8,'-ld3d9','-ldinput','-ldinput8','-ldsound')}
            & $compiler @args
            if($LASTEXITCODE){throw "Cannot build $name"}
            $outputs+=$output;Write-Host "Built $name ($target)"
        }
        foreach($output in $outputs){
            $imports=& (Join-Path $toolchain 'objdump.exe') -p $output|Select-String 'DLL Name:'
            if($LASTEXITCODE -or $imports -match 'libgcc_s|libstdc\+\+|libwinpthread'){throw "Invalid loader probe imports: $output"}
        }
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
