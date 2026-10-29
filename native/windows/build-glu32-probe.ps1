param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-10\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{foreach($bits in @(32,64)){
    $bin="C:\msys64\mingw$bits\bin";$env:PATH="$bin;$savedPath"
    $tmp=Join-Path $PSScriptRoot "build\glu32-probe$bits";New-Item -ItemType Directory -Force -Path $tmp|Out-Null;$env:TEMP=$tmp;$env:TMP=$tmp
    $expected=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& "$bin\g++.exe" -dumpmachine) -ne $expected){throw 'Unexpected GLU fixture compiler architecture'}
    foreach($static in @($false,$true)){foreach($preference in @('default','discrete')){
        $kind=if($static){'import'}else{'dynamic'};$exe=Join-Path $OutputDirectory "glu32-$kind-$preference$bits.exe"
        $options=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-municode',"$PSScriptRoot\glu32-probe.cpp",'-o',$exe,'-lopengl32','-lgdi32','-luser32','-lpsapi')
        if($static){$options+=@('-DSTATIC_GLU','-lglu32')};if($preference -eq 'discrete'){$options+='-DPREFER_DISCRETE'}
        & "$bin\g++.exe" @options;if($LASTEXITCODE){throw "GLU fixture compilation failed: $exe"}
        $imports=& "$bin\objdump.exe" -p $exe|Select-String 'DLL Name:'
        if($LASTEXITCODE -or $imports -match 'libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin'){throw "Unexpected GLU fixture runtime dependency: $exe"}
        Write-Host "Built glu32-$kind-$preference$bits.exe"
    }}
}}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
