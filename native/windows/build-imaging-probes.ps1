param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
        $bits=if($a -eq 'x86'){32}else{64};$tools="C:\msys64\mingw$bits\bin"
        $env:Path="$tools;$savedPath";$temporary=Join-Path $PSScriptRoot "build\imaging-$a"
        New-Item -ItemType Directory -Force -Path $temporary,$OutputDirectory|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $target=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& "$tools\g++.exe" -dumpmachine) -ne $target){throw 'Unexpected compiler architecture'}
        foreach($name in @('wic-probe','d3dx-image-probe','imaging-defaults-probe')){
            $output=Join-Path $OutputDirectory "$name$bits.exe"
            $sources=@("$PSScriptRoot\$name.cpp")
            if($name -eq 'imaging-defaults-probe'){$sources+=@("$PSScriptRoot\imaging-defaults.cpp","$PSScriptRoot\registry-views.cpp")}
            & "$tools\g++.exe" -std=c++17 -O2 -g -Wall -Wextra -Wno-cast-function-type -Wno-misleading-indentation -static -static-libgcc -static-libstdc++ -municode @sources -o $output -lole32 -loleaut32 -luuid -luser32 -ladvapi32
            if($LASTEXITCODE){throw "Failed to build $name$bits.exe"}
            $imports=& "$tools\objdump.exe" -p $output | Select-String 'DLL Name:'
            if($LASTEXITCODE -or $imports -match 'msys|cygwin|libgcc_s|libstdc\+\+|libwinpthread'){throw 'Unexpected runtime dependencies'}
            Write-Host "Built $name$bits.exe ($target; static C/C++ runtimes)"
        }
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
