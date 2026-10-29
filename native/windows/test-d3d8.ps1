param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-d3d8-probe.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\d3d8-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'D3D8 prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){
    foreach($name in @("d3d8-probe$bits.exe","d3d8-import-probe$bits.exe")){Copy-Item -LiteralPath (Join-Path "$PSScriptRoot\bin" $name) -Destination $directory}
}
# Make the hardware evidence explicit: WineD3D uses the host's OpenGL driver.
$registry="Windows Registry Editor Version 5.00`r`n`r`n[HKEY_CURRENT_USER\Software\Wine\Direct3D]`r`n`"renderer`"=`"gl`"`r`n"
$regFile=Join-Path $prefix 'renderer.reg';[IO.File]::WriteAllText($regFile,$registry)
& $runtime --prefix $prefix reg import $regFile
if($LASTEXITCODE){throw 'D3D8 renderer setup failed'}
$before=& "$PSScriptRoot\bin\display-probe64.exe" | Where-Object {$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Cannot snapshot host display mode'}
$before|Set-Content -LiteralPath (Join-Path $prefix 'host-before.log')
$saved=$env:WINEDLLOVERRIDES
$results=@()
try {
    foreach($bits in @(32,64)){
        $native=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\d3d8.dll'}else{'System32\d3d8.dll'})
        $providers=@('wine')
        if(Test-Path -LiteralPath $native){$providers+='native'}else{
            $results+=@{architecture=$bits;provider='native';status='unavailable';reason='Host does not supply this D3D8 architecture';path=$native}
            Write-Host "UNAVAILABLE native D3D8 $bits-bit: $native"
            $providers+='wine-fallback'
        }
        foreach($provider in $providers){
            $env:WINEDLLOVERRIDES=if($provider -eq 'wine'){'d3d8=b,n'}else{'d3d8=n,b'}
            foreach($desktop in @($false,$true)){
                foreach($static in @($false,$true)){
                    $name=if($static){"d3d8-import-probe$bits.exe"}else{"d3d8-probe$bits.exe"}
                    $arguments=@('--prefix',$prefix)
                    if($desktop){$arguments+=@('--desktop','D3D8,800x600')}else{$arguments+=@('--desktop','off')}
                    $arguments+=@('run',"C:\tests\$name")
                    if($provider -eq 'native'){$arguments+='--native'}
                    $label="$provider-$bits-desktop$desktop-static$static"
                    & $runtime @arguments 2>&1|Tee-Object -FilePath (Join-Path $prefix "$label.log")
                    if($LASTEXITCODE){throw "D3D8 fixture failed: $label"}
                    $results+=@{architecture=$bits;provider=$provider;virtualDesktop=$desktop;staticImport=$static;status='passed'}
                }
            }
        }
    }
    $after=& "$PSScriptRoot\bin\display-probe64.exe" | Where-Object {$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $after -ne $before){throw "Host display state changed: $before / $after"}
    @{passed=$true;hostDisplayPreserved=$true;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
} finally {$env:WINEDLLOVERRIDES=$saved}
Write-Host "D3D8 suite passed (unavailable providers are explicitly recorded): $prefix"
$global:LASTEXITCODE=0
