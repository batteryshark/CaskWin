param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\dpi-08\fixtures'))
$ErrorActionPreference='Stop';$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {foreach($bits in @(32,64)){
    $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
    $temporary=Join-Path $PSScriptRoot "build\desktop-dpi$bits";New-Item -ItemType Directory -Force -Path $temporary|Out-Null
    $env:TEMP=$temporary;$env:TMP=$temporary
    $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
    if((& "$toolchain\g++.exe" -dumpmachine) -ne $target){throw 'Wrong desktop DPI fixture architecture'}
    foreach($mode in @('unaware','pm')){
        $dpi=if($mode -eq 'pm'){'True/PM'}else{'False'}
        $manifest=@"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<assemblyIdentity version="1.0.0.0" processorArchitecture="*" name="Winedows.OwnedDpiProbe" type="win32"/>
<trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo>
<compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application></compatibility>
<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings><dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">$dpi</dpiAware></windowsSettings></application>
</assembly>
"@
        $manifest|Set-Content -LiteralPath "$temporary\probe.manifest" -Encoding utf8
        ('1 24 "'+("$temporary\probe.manifest".Replace('\','/'))+'"')|Set-Content -LiteralPath "$temporary\probe.rc" -Encoding ascii
        & "$toolchain\windres.exe" "$temporary\probe.rc" -O coff -o "$temporary\resource.o"
        if($LASTEXITCODE){throw 'Desktop DPI manifest resource failed'}
        & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode "$PSScriptRoot\desktop-dpi-probe.cpp" "$temporary\resource.o" -o (Join-Path $OutputDirectory "desktop-dpi-$mode$bits.exe") -luser32 -lgdi32
        if($LASTEXITCODE){throw 'Desktop DPI fixture build failed'}
    }
    & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode "$PSScriptRoot\desktop-dpi-host-probe.cpp" -o (Join-Path $OutputDirectory "desktop-dpi-host$bits.exe") -luser32
    if($LASTEXITCODE){throw 'Desktop DPI native hosting fixture build failed'}
    # The final resource above contains the same True/PM declaration as Unity.
    & "$toolchain\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -municode -mwindows "$PSScriptRoot\desktop-dpi-gui-probe.cpp" "$temporary\resource.o" -o (Join-Path $OutputDirectory "desktop-dpi-gui$bits.exe") -luser32 -lshell32
    if($LASTEXITCODE){throw 'Desktop DPI GUI fixture build failed'}
}} finally {$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
