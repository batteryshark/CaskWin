param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-d3dx9-probe.ps1"}
$suite=Join-Path $PSScriptRoot ('test-output\d3dx9-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix'
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'D3DX9 prefix initialization failed'}
$programs=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $programs|Out-Null
foreach($bits in @(32,64)){foreach($kind in @('d3dx9-probe','d3dx9-import-probe')){Copy-Item -LiteralPath "$PSScriptRoot\bin\$kind$bits.exe" -Destination $programs}}
$before=& "$PSScriptRoot\bin\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Cannot snapshot host display mode'}
$before|Set-Content -LiteralPath (Join-Path $suite 'host-before.log')
$saved=$env:WINEDLLOVERRIDES
$results=[Collections.Generic.List[object]]::new()
$nativeAvailable=@{}
function Invoke-D3dxFixture([string]$label,[string]$loader,[int]$bits,[bool]$static,[bool]$desktop,[string]$expected){
    $kind=if($static){'d3dx9-import-probe'}else{'d3dx9-probe'}
    $arguments=@('--prefix',$prefix,'--desktop',$(if($desktop){'D3DX9,800x600'}else{'off'}),'run',"C:\tests\$kind$bits.exe",'--expected-dll',$expected)
    & $loader @arguments 2>&1|Tee-Object -FilePath (Join-Path $suite "$label.log")
    $code=$LASTEXITCODE
    $results.Add(@{fixture=$label;architecture=$bits;staticImport=$static;virtualDesktop=$desktop;expectedProvider=$expected;exitCode=$code;status=$(if($code){'failed'}else{'passed'})})
    if($code){throw "D3DX9 fixture failed: $label (exit $code)"}
}
try {
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $native=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\d3dx9_43.dll'}else{'System32\d3dx9_43.dll'})
        $nativeAvailable[$bits]=Test-Path -LiteralPath $native
        $providers=@('wine')
        if($nativeAvailable[$bits]){$providers+='native'}else{$results.Add(@{architecture=$bits;provider='native';status='unavailable';reason='Host does not supply the optional legacy D3DX9_43 runtime';path=$native})}
        foreach($provider in $providers){
            foreach($graphics in @('wine','native')){
                $helperOrder=if($provider -eq 'wine'){'b,n'}else{'n,b'}
                $graphicsOrder=if($graphics -eq 'wine'){'b,n'}else{'n,b'}
                $env:WINEDLLOVERRIDES="d3dx9_43=$helperOrder;d3d9=$graphicsOrder"
                $expected=if($provider -eq 'wine'){"$PSScriptRoot\bin\builtin\$arch\d3dx9_43.dll"}else{$native}
                foreach($desktop in @($false,$true)){foreach($static in @($false,$true)){
                    $label="$provider-helper-$graphics-d3d9-$bits-desktop$desktop-static$static"
                    Invoke-D3dxFixture $label $runtime $bits $static $desktop $expected
                }}
            }
        }
    }
    # Exercise availability fallback using a private runtime copy. Never remove
    # or rename a live shared builtin or a game's DLL.
    $fallback=Join-Path $suite 'runtime-without-d3dx9'
    New-Item -ItemType Directory -Path $fallback|Out-Null
    foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){Copy-Item -LiteralPath "$PSScriptRoot\bin\$name" -Destination $fallback}
    foreach($arch in @('x86','x64')){
        $destination=Join-Path $fallback "builtin\$arch";New-Item -ItemType Directory -Path $destination|Out-Null
        Get-ChildItem -LiteralPath "$PSScriptRoot\bin\builtin\$arch" -File|Where-Object {$_.Name -ne 'd3dx9_43.dll'}|ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $destination}
    }
    $fallbackLoader=Join-Path $fallback 'wine64.exe'
    foreach($bits in @(32,64)){
        $native=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\d3dx9_43.dll'}else{'System32\d3dx9_43.dll'})
        if($nativeAvailable[$bits]){
            $env:WINEDLLOVERRIDES='d3dx9_43=b,n;d3d9=n'
            Invoke-D3dxFixture "missing-builtin-native-fallback-$bits" $fallbackLoader $bits $true $false $native
        }
        $env:WINEDLLOVERRIDES='d3dx9_43=b;d3d9=n'
        $log=Join-Path $suite "missing-strict-builtin-$bits.log"
        & $fallbackLoader --prefix $prefix run "C:\tests\d3dx9-probe$bits.exe" --expected-dll $native *> $log
        $code=$LASTEXITCODE;$text=Get-Content -Raw -LiteralPath $log
        $rejected=$code -ne 0 -and $text -match 'No available implementation.*d3dx9_43'
        $results.Add(@{fixture="missing-strict-builtin-$bits";architecture=$bits;exitCode=$code;status=$(if($rejected){'rejected-as-required'}else{'failed'})})
        if(!$rejected){throw 'Strict missing D3DX builtin did not produce the expected loader error'}
    }
    $after=& "$PSScriptRoot\bin\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $after -ne $before){throw 'Host display state changed during D3DX9 tests'}
    @{passed=$true;hostDisplayPreserved=$true;cases=$results;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "D3DX9 suite passed: $suite"
} catch {
    @{passed=$false;error=$_.Exception.Message;cases=$results;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
} finally {$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
