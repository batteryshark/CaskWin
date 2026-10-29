param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\display-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix init failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory | Out-Null
$saved=$env:WINEDLLOVERRIDES
try {
    foreach($bits in @(32,64)){
        Copy-Item -LiteralPath "$PSScriptRoot\bin\display-probe$bits.exe" -Destination (Join-Path $directory "display$bits.exe")
        $before=& "$PSScriptRoot\bin\display-probe$bits.exe" | Where-Object {$_ -like 'DISPLAY *'}
        if($LASTEXITCODE -or !$before){throw 'Cannot query host display state'}
        $before|Set-Content -LiteralPath (Join-Path $prefix "host-before-$bits.log")
        $env:WINEDLLOVERRIDES='ddraw,d3d9=b'
        & $runtime --prefix $prefix run "C:\tests\display$bits.exe" --wine 2>&1 | Tee-Object -FilePath (Join-Path $prefix "wine-$bits.log")
        if($LASTEXITCODE){throw "Wine display fixture failed ($bits-bit)"}
        $env:WINEDLLOVERRIDES='ddraw,d3d9=n'
        $native=& $runtime --prefix $prefix run "C:\tests\display$bits.exe" | Where-Object {$_ -like 'DISPLAY *'}
        if($LASTEXITCODE -or $native -ne $before){throw 'Native fallback changed the display API view'}
        $after=& "$PSScriptRoot\bin\display-probe$bits.exe" | Where-Object {$_ -like 'DISPLAY *'}
        if($LASTEXITCODE -or $after -ne $before){throw 'Host display state changed'}
    }
    @{passed=$true;architectures=@('x86','x64');hostDisplayPreserved=$true;nativeFallbackPreserved=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
} finally {$env:WINEDLLOVERRIDES=$saved}
Write-Host "Display tests passed: $prefix"
