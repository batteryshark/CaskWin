param([ValidateSet('x86','x64','all')][string]$Arch='all',[string]$WineSourceDirectory,
      [string]$OutputDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
. "$PSScriptRoot\wine-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$savedPath=$env:Path;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
try {
    foreach($a in $arches){
        $wineBuild=Assert-CaskWinWineBuild -WineSourceDirectory $wineSource -Arch $a
        $bits=if($a -eq 'x86'){32}else{64};$toolchain="C:\msys64\mingw$bits\bin";$compiler=Join-Path $toolchain 'g++.exe'
        $env:Path="$toolchain;$savedPath";$temp=Join-Path $PSScriptRoot "build\xaudio-probe-$a"
        New-Item -ItemType Directory -Force -Path $temp,$OutputDirectory|Out-Null
        $env:TEMP=$temp;$env:TMP=$temp
        $target=if($a -eq 'x86'){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $target){throw "Wrong compiler target for $a"}
        $widl=Join-Path $wineBuild 'tools\widl\widl.exe'
        if(!(Test-Path -LiteralPath $widl)){throw 'Build the native Wine DLL targets first (widl is required for version-specific XAudio interfaces).'}
        foreach($version in @(7,8,9)){
            $headers=Join-Path $temp "v$version";New-Item -ItemType Directory -Force -Path $headers|Out-Null
            # Generate each ABI from Wine's original IDL; 2.7 has different vtables.
            & $widl '-h' '-o' (Join-Path $headers 'xaudio-probe-api.h') "-DXAUDIO2_VER=$version" "-I$wineSource\include" "$wineSource\dlls\xaudio2_7\xaudio_classes.idl"
            if($LASTEXITCODE){throw "Generate XAudio2.$version header failed ($a)"}
            $variants=if($version -eq 7){@($false)}else{@($false,$true)}
            foreach($static in $variants){
                $kind=if($static){'import-probe'}else{'probe'};$name="xaudio$version-$kind$bits.exe";$output=Join-Path $OutputDirectory $name
                $flags=@('-std=c++17','-O2','-g','-Wall','-Wextra','-Wno-cast-function-type','-Wno-unused-function','-static','-static-libgcc','-static-libstdc++','-municode','-DUNICODE','-D_UNICODE',"-DXAUDIO2_VER=$version","-I$headers","$PSScriptRoot\xaudio-probe.cpp",'-o',$output,'-lole32','-luuid','-luser32','-ladvapi32')
                if($static){$flags+=@('-DWINEDOWS_STATIC_XAUDIO_IMPORT',"-lxaudio2_$version")}
                & $compiler @flags
                if($LASTEXITCODE){throw "Build failed: $name"}
                $imports=& (Join-Path $toolchain 'objdump.exe') -p $output | Select-String 'DLL Name:'
                if($imports -match 'libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin'){throw "Nonportable runtime dependency: $name"}
                Write-Host "Built $name ($target; static C/C++ runtimes)"
            }
        }
    }
}finally{$env:Path=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
