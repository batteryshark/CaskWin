param([switch]$SkipBuild,[string]$RuntimeDirectory,[string]$FixtureDirectory)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$FixtureDirectory){$FixtureDirectory=$RuntimeDirectory}
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory}
$prefix=Join-Path $PSScriptRoot ('test-output\desktop-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix init failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){
    foreach($probe in @('desktop','display','wine-graphics')){Copy-Item -LiteralPath "$FixtureDirectory\$probe-probe$bits.exe" -Destination (Join-Path $directory "$probe$bits.exe")}
}
$before=& "$FixtureDirectory\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Host display query failed'}
$before|Set-Content -LiteralPath (Join-Path $prefix 'host-before.log')
$saved=$env:WINEDLLOVERRIDES
try {
    foreach($bits in @(32,64)){
        $other=if($bits -eq 32){64}else{32}
        $env:WINEDLLOVERRIDES='ddraw,d3d9=b'
        & $runtime --prefix $prefix --desktop 'Test,800x600' run "C:\tests\desktop$bits.exe" --child "C:\tests\desktop$other.exe" 2>&1|Tee-Object -FilePath (Join-Path $prefix "windows-$bits.log")
        if($LASTEXITCODE){throw "Virtual desktop window/child fixture failed ($bits-bit)"}
        & $runtime --prefix $prefix explorer /desktop=Test,640x480 "C:\tests\display$bits.exe" --wine 2>&1|Tee-Object -FilePath (Join-Path $prefix "graphics-$bits.log")
        if($LASTEXITCODE){throw "Virtual desktop Wine graphics failed ($bits-bit)"}
        $env:WINEDLLOVERRIDES='ddraw,d3d9=n'
        & $runtime --prefix $prefix --desktop 'Test,800x600' run "C:\tests\wine-graphics$bits.exe" --native 2>&1|Tee-Object -FilePath (Join-Path $prefix "native-$bits.log")
        if($LASTEXITCODE){throw "Virtual desktop native graphics failed ($bits-bit)"}
    }
    $registry=@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\Explorer]
"Desktop"="Default"

[HKEY_CURRENT_USER\Software\Wine\Explorer\Desktops]
"Default"="800x600"
"Alternate"="1024x768"

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\display32.exe\Explorer]
"Desktop"="Alternate"
'@
    $regFile=Join-Path $prefix 'desktop.reg';[IO.File]::WriteAllText($regFile,$registry)
    & $runtime --prefix $prefix reg import $regFile
    if($LASTEXITCODE){throw 'Desktop settings import failed'}
    & $runtime --prefix $prefix run 'C:\tests\desktop64.exe' 2>&1|Tee-Object -FilePath (Join-Path $prefix 'registry.log')
    if($LASTEXITCODE){throw 'Persistent Wine desktop setting failed'}
    $app=& $runtime --prefix $prefix run 'C:\tests\display32.exe'|Where-Object {$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $app -notlike 'DISPLAY 1024 768 32 *'){throw 'Per-application desktop setting did not win'}
    $disabled=& $runtime --prefix $prefix --desktop off run 'C:\tests\display32.exe'|Where-Object {$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $disabled -ne $before){throw 'Explicit desktop opt-out did not restore host view'}
    $after=& "$FixtureDirectory\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $after -ne $before){throw "Host display changed: $before / $after"}
    @{passed=$true;architectures=@('x86','x64');hostDisplayPreserved=$true;wineAndNativeGraphics=$true;crossBitnessChildren=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
} finally {$env:WINEDLLOVERRIDES=$saved}
Write-Host "Virtual desktop tests passed: $prefix"
