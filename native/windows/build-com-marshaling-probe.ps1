param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})) {
        $bits=if($a -eq 'x86'){'32'}else{'64'}
        $toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
        $env:Path="$toolchain;$savedPath"
        $temp=Join-Path $PSScriptRoot "build\com-marshaling-probe\$a";New-Item -ItemType Directory -Force -Path $temp|Out-Null
        $env:TEMP=$temp;$env:TMP=$temp
        $target=& $compiler -dumpmachine
        $expected=if($bits -eq '32'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if($LASTEXITCODE -or $target -ne $expected){throw "Unexpected compiler target $target"}
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-static','-static-libgcc','-static-libstdc++','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0602')
        & $compiler @flags -municode "$PSScriptRoot\com-marshaling-probe.cpp" -o (Join-Path $OutputDirectory "com-marshaling-probe$bits.exe") -ladvapi32 -lole32 -luuid
        if($LASTEXITCODE){throw "COM marshaling fixture compilation failed: $a"}
        & $compiler @flags -c "$PSScriptRoot\com-marshaling.cpp" -o (Join-Path $temp 'com-marshaling.o')
        if($LASTEXITCODE){throw "COM defaults helper compilation failed: $a"}
        $imports=& (Join-Path $toolchain 'objdump.exe') -p (Join-Path $OutputDirectory "com-marshaling-probe$bits.exe") | Select-String 'DLL Name'
        if($imports -match 'libgcc|libstdc\+\+|libwinpthread'){throw 'Fixture has a dynamic MinGW runtime dependency'}
        Write-Host "COM marshaling fixture built: $a"
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
