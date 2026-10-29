param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory)
$ErrorActionPreference='Stop'
if(!$FixtureDirectory){$FixtureDirectory=$RuntimeDirectory}
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-wine.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\overrides-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix init failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory | Out-Null
foreach($bits in @(32,64)){
    Copy-Item -LiteralPath "$FixtureDirectory\wine-import-probe$bits.exe" -Destination (Join-Path $directory "imports$bits.exe")
    Copy-Item -LiteralPath "$FixtureDirectory\wine-graphics-probe$bits.exe" -Destination (Join-Path $directory "dynamic$bits.exe")
}
$saved=$env:WINEDLLOVERRIDES
try {
    $env:WINEDLLOVERRIDES='d3d8=n'
    & $runtime --prefix $prefix run 'C:\tests\imports64.exe' --native
    if($LASTEXITCODE){throw 'An unused native DLL override blocked application startup'}
    foreach($mode in @('b,n','n,b')){
        $env:WINEDLLOVERRIDES="ddraw,d3d9=$mode"
        foreach($bits in @(32,64)){
            $other=if($bits -eq 32){64}else{32}
            $arguments=@('--prefix',$prefix,'run',"C:\tests\imports$bits.exe")
            if($mode -eq 'n,b'){$arguments+='--native'}
            $arguments+=@('--child-program',"C:\tests\dynamic$other.exe")
            & $runtime @arguments 2>&1 | Tee-Object -FilePath (Join-Path $prefix "order-$($mode.Replace(',',''))-$bits.log")
            if($LASTEXITCODE){throw "DLL order $mode failed on $bits-bit"}
        }
    }
    foreach($bits in @(32,64)){
        $env:WINEDLLOVERRIDES='ddraw=b;d3d9=n'
        & $runtime --prefix $prefix run "C:\tests\imports$bits.exe" --native-d3d9
        if($LASTEXITCODE){throw 'Mixed builtin DirectDraw and native Direct3D9 failed'}
        $env:WINEDLLOVERRIDES='ddraw=n;d3d9=b'
        & $runtime --prefix $prefix run "C:\tests\imports$bits.exe" --native-ddraw
        if($LASTEXITCODE){throw 'Mixed native DirectDraw and builtin Direct3D9 failed'}
    }
    # Standard Wine registry locations, with application settings above prefix defaults.
    $env:WINEDLLOVERRIDES=$null
    $registry=@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"ddraw"="native,builtin"
"d3d9"="native,builtin"

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\imports32.exe\DllOverrides]
"ddraw"="builtin,native"
"d3d9"="builtin,native"
'@
    $regFile=Join-Path $prefix 'overrides.reg'
    [IO.File]::WriteAllText($regFile,$registry)
    & $runtime --prefix $prefix reg import $regFile
    if($LASTEXITCODE){throw 'Cannot import override fixture'}
    & $runtime --prefix $prefix run 'C:\tests\imports32.exe'
    if($LASTEXITCODE){throw 'Application override did not take precedence'}
    & $runtime --prefix $prefix run 'C:\tests\imports64.exe' --native
    if($LASTEXITCODE){throw 'Prefix default override failed'}
    $env:WINEDLLOVERRIDES='ddraw,d3d9=n'
    & $runtime --prefix $prefix run 'C:\tests\imports32.exe' --native
    if($LASTEXITCODE){throw 'Environment override did not take precedence'}
    # A separate loader distribution without builtins proves availability fallback.
    $minimal=Join-Path $prefix '.winedows\runtime-without-builtins'
    New-Item -ItemType Directory -Path $minimal | Out-Null
    foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $minimal}
    $env:WINEDLLOVERRIDES='ddraw,d3d9=b,n'
    & (Join-Path $minimal 'wine64.exe') --prefix $prefix run 'C:\tests\imports64.exe' --native
    if($LASTEXITCODE){throw 'Missing-builtin fallback to native failed'}
    $env:WINEDLLOVERRIDES='ddraw,d3d9=b'
    & (Join-Path $minimal 'wine64.exe') --prefix $prefix run 'C:\tests\imports64.exe'
    if(!$LASTEXITCODE){throw 'Strict builtin selection silently fell back to native'}
    if(Test-Path -LiteralPath (Join-Path $directory 'ddraw.dll')){throw 'Loader must not copy provider DLLs into the application directory'}
    @{passed=$true;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "DLL override suite passed: $prefix"
} finally {$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
