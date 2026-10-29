param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-d3d11-probe.ps1" -OutputDirectory $RuntimeDirectory}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$prefix=Join-Path $PSScriptRoot ('test-output\d3d11-'+[Guid]::NewGuid().ToString('N'))
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'D3D11 prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Force -Path $directory|Out-Null
foreach($bits in @(32,64)){foreach($name in @("d3d11-probe$bits.exe","d3d11-import-probe$bits.exe","d3d10-probe$bits.exe")){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $directory}}
$reg=Join-Path $prefix 'renderer.reg'
@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\Direct3D]
"renderer"="gl"
'@|Set-Content -LiteralPath $reg
& $runtime --prefix $prefix reg import $reg
if($LASTEXITCODE){throw 'D3D11 renderer setup failed'}
$before=& (Join-Path $RuntimeDirectory 'display-probe64.exe')|Where-Object{$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Cannot read host display mode'}
$saved=$env:WINEDLLOVERRIDES;$savedDump=$env:D3D11_PROBE_DUMP;$results=@()
try {
    $providers=@(
        @{label='wine';overrides='dxgi,d3d11=b';expected='wine'},
        @{label='native';overrides='dxgi,d3d11=n';expected='native'},
        @{label='native-fallback';overrides='dxgi=n;d3d11=b,n';expected='native'}
    )
    foreach($bits in @(32,64)){
        foreach($provider in $providers){
            $env:WINEDLLOVERRIDES=$provider.overrides
            foreach($desktop in @($false,$true)){
                foreach($static in @($false,$true)){
                    $name=if($static){"d3d11-import-probe$bits.exe"}else{"d3d11-probe$bits.exe"}
                    $label="$($provider.label)-$bits-desktop$desktop-static$static"
                    $env:D3D11_PROBE_DUMP=Join-Path $prefix "$label.dmp"
                    $desktopOption=if($desktop){'D3D11,800x600'}else{'off'}
                    & $runtime --prefix $prefix --desktop $desktopOption run "C:\tests\$name" "--$($provider.expected)" 2>&1|Tee-Object -FilePath (Join-Path $prefix "$label.log")
                    if($LASTEXITCODE){throw "D3D11 fixture failed: $label"}
                    $results+=@{api='D3D11';architecture=$bits;provider=$provider.label;virtualDesktop=$desktop;staticImport=$static;status='passed'}
                }
                foreach($version1 in @($false,$true)){
                    $api=if($version1){'D3D10.1'}else{'D3D10'}
                    $label="$api-$($provider.label)-$bits-desktop$desktop"
                    $apiOption=if($version1){'--10-1'}else{'--10-0'}
                    & $runtime --prefix $prefix --desktop $desktopOption run "C:\tests\d3d10-probe$bits.exe" "--$($provider.expected)" $apiOption 2>&1|Tee-Object -FilePath (Join-Path $prefix "$label.log")
                    if($LASTEXITCODE){throw "D3D10 fixture failed: $label"}
                    $results+=@{api=$api;architecture=$bits;provider=$provider.label;virtualDesktop=$desktop;staticImport=$false;status='passed'}
                }
            }
        }
        $env:WINEDLLOVERRIDES='dxgi=n;d3d11=b'
        & $runtime --prefix $prefix --desktop off run "C:\tests\d3d11-probe$bits.exe" --wine 2>&1|Tee-Object -FilePath (Join-Path $prefix "strict-incompatible-$bits.log")
        if(!$LASTEXITCODE){throw 'Strict incompatible Wine D3D11/native DXGI unexpectedly started'}
        $results+=@{architecture=$bits;provider='strict-incompatible';status='rejected'}
    }
    $after=& (Join-Path $RuntimeDirectory 'display-probe64.exe')|Where-Object{$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $after -ne $before){throw "Host display changed: $before / $after"}
    @{passed=$true;hostDisplayPreserved=$true;shaderCompiler='native system d3dcompiler_47';videoDecodingExercised=$false;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
}finally{$env:WINEDLLOVERRIDES=$saved;$env:D3D11_PROBE_DUMP=$savedDump}
Write-Host "D3D11 suite passed: $prefix"
$global:LASTEXITCODE=0
