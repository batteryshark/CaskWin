param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\vulkan-loader-probes'))
$ErrorActionPreference='Stop'
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP
try{
    foreach($bits in @(32,64)){
        $toolchain="C:\msys64\mingw$bits\bin";$env:PATH="$toolchain;$savedPath"
        $temporary=Join-Path $PSScriptRoot "build\vulkan-loader-probe$bits"
        New-Item -ItemType Directory -Force -Path $temporary|Out-Null
        $env:TEMP=$temporary;$env:TMP=$temporary
        $compiler=Join-Path $toolchain 'g++.exe'
        $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        if((& $compiler -dumpmachine) -ne $target){throw 'Wrong Vulkan fixture compiler target'}
        $flags=@('-std=c++17','-O2','-Wall','-Wextra','-Wno-cast-function-type','-static','-static-libgcc','-static-libstdc++','-D_WIN32_WINNT=0x0602')
        foreach($marker in @($false,$true)){
            $name=if($marker){"vulkan-loader-marker$bits.dll"}else{"vulkan-loader-probe$bits.exe"}
            $extra=if($marker){@('-shared','-DWINEDOWS_VULKAN_LOADER_MARKER','-Wl,--kill-at')}else{@('-municode')}
            $arguments=$flags+@($extra)+@("$PSScriptRoot\vulkan-loader-probe.cpp",'-lpsapi','-o',(Join-Path $OutputDirectory $name))
            & $compiler @arguments
            if($LASTEXITCODE){throw "Vulkan loader fixture build failed: $name"}
            $imports=& (Join-Path $toolchain 'objdump.exe') -p (Join-Path $OutputDirectory $name)|Select-String 'DLL Name:'
            if($LASTEXITCODE -or $imports -match 'msys|cygwin|libgcc_s|libstdc\+\+|libwinpthread'){throw "Unexpected fixture runtime dependencies: $name"}
            Write-Host "Built $name ($target; static C/C++ runtimes)"
        }
    }
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp}
$global:LASTEXITCODE=0
