param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\xact-08\fixtures'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
. "$PSScriptRoot\wine-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$headers=Join-Path $OutputDirectory 'include'
New-Item -ItemType Directory -Force -Path $OutputDirectory,$headers|Out-Null
Copy-Item -LiteralPath "$wineSource\include\xact3.h","$wineSource\include\xact3wb.h" -Destination $headers
$source=[IO.File]::ReadAllText("$wineSource\dlls\xactengine3_7\tests\xact3.c")
$data="// LGPL-2.1-or-later. Generated from Wine's xactengine3_7 tests.`n"
foreach($name in @('soundbank_data','global_settings_data')){
    $match=[regex]::Match($source,"static const char $name\[\]\s*=\s*\{(?<data>.*?)\};",'Singleline')
    if(!$match.Success){throw "Upstream XACT test data not found: $name"}
    $data+="static const unsigned char $name`[`] = {"+$match.Groups['data'].Value+"};`n"
}
[IO.File]::WriteAllText((Join-Path $headers 'xact-fixture-data.h'),$data)
Copy-Item -LiteralPath "$wineSource\dlls\xactengine3_7\tests\test.xwb" -Destination (Join-Path $OutputDirectory 'test.xwb')
& C:\msys64\mingw64\bin\python.exe "$PSScriptRoot\make-xact-fixtures.py" "$wineSource\dlls\xactengine3_7\tests\test.xwb" (Join-Path $OutputDirectory 'streaming.xwb')
if($LASTEXITCODE){throw 'XACT streaming fixture generation failed'}
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    $env:TEMP=$OutputDirectory;$env:TMP=$OutputDirectory
    foreach($bits in @(32,64)){
        $tool="C:\msys64\mingw$bits\bin";$env:PATH="$tool;$savedPath"
        $expected=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& "$tool\g++.exe" -dumpmachine) -ne $expected){throw 'Wrong XACT compiler target'}
        & "$tool\g++.exe" -std=c++17 -O2 -g -Wall -Wextra -Wno-cast-function-type -Wno-misleading-indentation -static -static-libgcc -static-libstdc++ -municode -D_WIN32_WINNT=0x0602 -DXACT3_VER=0x0307 "-I$headers" "$PSScriptRoot\xact-probe.cpp" -o "$OutputDirectory\xact-probe$bits.exe" -lole32 -luuid -ladvapi32
        if($LASTEXITCODE){throw "XACT fixture compilation failed ($bits)"}
        $imports=& "$tool\objdump.exe" -p "$OutputDirectory\xact-probe$bits.exe" | Select-String 'DLL Name:'
        if($imports -match 'msys|cygwin|libgcc_s|libstdc\+\+|libwinpthread'){throw 'Nonportable fixture runtime'}
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
