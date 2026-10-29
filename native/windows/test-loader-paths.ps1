param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-loader-paths-probe.ps1"}
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
$prefix=Join-Path $PSScriptRoot ('test-output\loader-paths-'+[Guid]::NewGuid().ToString('N'))
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Loader-paths prefix initialization failed'}
$saved=$env:WINEDLLOVERRIDES;$results=@()
try {
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $directory=Join-Path $prefix "drive_c\tests$bits";$resources=Join-Path $directory 'resources'
        New-Item -ItemType Directory -Force -Path $resources|Out-Null
        foreach($dll in @('ddraw','d3d8','d3d9','dinput','dinput8','dsound')){Copy-Item -LiteralPath "$PSScriptRoot\bin\loader-paths-marker$bits.dll" -Destination (Join-Path $resources "$dll.dll")}
        foreach($static in @($false,$true)){
            $name=if($static){"loader-paths-import-probe$bits.exe"}else{"loader-paths-probe$bits.exe"}
            Copy-Item -LiteralPath (Join-Path "$PSScriptRoot\bin" $name) -Destination $directory
            foreach($provider in @('wine','native')){
                # Static imports cannot start when the host has no native x64 D3D8.
                if($static -and $provider -eq 'native' -and $bits -eq 64 -and !(Test-Path "$env:WINDIR\System32\d3d8.dll")){
                    $results+=@{architecture=$bits;staticImport=$true;provider='native';status='unavailable';reason='Host lacks native x64 D3D8 required by static fixture'};continue
                }
                $env:WINEDLLOVERRIDES=if($provider -eq 'wine'){'ddraw,d3d8,d3d9,dinput,dinput8,dsound=b,n'}else{'ddraw,d3d8,d3d9,dinput,dinput8,dsound=n'}
                $label="$provider-$bits-static$static"
                & $runtime --prefix $prefix run "C:\tests$bits\$name" "--$provider" "$PSScriptRoot\bin\builtin\$arch" 2>&1|Tee-Object -FilePath (Join-Path $prefix "$label.log")
                if($LASTEXITCODE){throw "Loader-paths fixture failed: $label"}
                $results+=@{architecture=$bits;staticImport=$static;provider=$provider;status='passed'}
            }
        }
    }
    @{passed=$true;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
}finally{$env:WINEDLLOVERRIDES=$saved}
Write-Host "Loader-paths suite passed: $prefix"
$global:LASTEXITCODE=0
