param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'bin'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try {
    foreach($a in $(if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)})){
        $bits=if($a -eq 'x86'){'32'}else{'64'}
        $toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
        $env:Path="$toolchain;$savedPath"
        $temporary=Join-Path $PSScriptRoot "build\xinput-probe\$a"
        New-Item -ItemType Directory -Force -Path $temporary|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $target=& $compiler -dumpmachine
        $expected=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if($LASTEXITCODE -or $target -ne $expected){throw "Unexpected XInput fixture compiler target: $target"}
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++','-municode','-DUNICODE','-D_UNICODE')
        $source=Join-Path $PSScriptRoot 'xinput-probe.cpp'
        & $compiler @flags $source -o (Join-Path $OutputDirectory "xinput-probe$bits.exe") -lsetupapi
        if($LASTEXITCODE){throw "XInput fixture compilation failed ($a)"}
        foreach($version in @('1_3','1_4','9_1_0')){
            $definition=Join-Path $temporary "xinput$version.def"
            $exports=if($a -eq 'x86'){@('XInputGetState@8','XInputGetCapabilities@12','XInputSetState@8')}else{@('XInputGetState','XInputGetCapabilities','XInputSetState')}
            @("LIBRARY xinput$version.dll",'EXPORTS')+$exports|Set-Content -LiteralPath $definition -Encoding ascii
            $library=Join-Path $temporary "libxinput$version.a"
            & (Join-Path $toolchain 'dlltool.exe') -k -d $definition -l $library
            if($LASTEXITCODE){throw "XInput import library failed ($a/$version)"}
            & $compiler @flags -DWINEDOWS_STATIC_XINPUT $source $library -o (Join-Path $OutputDirectory "xinput-import-$version-$bits.exe") -lsetupapi
            if($LASTEXITCODE){throw "XInput startup fixture failed ($a/$version)"}
        }
        Write-Host "XInput fixtures built: $a"
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
