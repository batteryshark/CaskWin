param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\d3dx9-version-probes'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
try {
    foreach($a in $arches){
        $toolchain=if($a -eq 'x86'){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $env:PATH="$toolchain;$savedPath"
        $temp=Join-Path $PSScriptRoot "build\d3dx9-version-probe-$a"
        New-Item -ItemType Directory -Force -Path $temp,$OutputDirectory|Out-Null
        $env:TEMP=$temp;$env:TMP=$temp
        $compiler=Join-Path $toolchain 'g++.exe'
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $expected){throw "Wrong $a compiler target"}
        $bits=if($a -eq 'x86'){32}else{64}
        foreach($version in @(37,41,42)){foreach($static in @($false,$true)){
            $name=if($static){"d3dx9-$version-import-probe$bits.exe"}else{"d3dx9-$version-probe$bits.exe"}
            $output=Join-Path $OutputDirectory $name
            $arguments=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-misleading-indentation','-D_WIN32_WINNT=0x0601','-DUNICODE','-D_UNICODE','-static','-static-libgcc','-static-libstdc++','-municode',"-DWINEDOWS_D3DX_VERSION=$version","$PSScriptRoot\d3dx9-version-probe.cpp",'-o',$output,'-luser32')
            if($static){$arguments+=@('-DWINEDOWS_STATIC_D3DX9_IMPORT',"-ld3dx9_$version")}
            & $compiler @arguments
            if($LASTEXITCODE){throw "D3DX9 probe build failed: $name"}
            $imports=& (Join-Path $toolchain 'objdump.exe') -p $output|Select-String 'DLL Name:'
            if($LASTEXITCODE){throw "Cannot inspect imports: $name"}
            if($imports -match 'libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin'){throw "Unexpected runtime dependency: $name"}
            Write-Host "Built $name ($expected; static C/C++ runtimes)"
        }}
    }
} finally {$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
