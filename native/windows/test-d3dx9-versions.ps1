param(
    [switch]$SkipBuild,
    [switch]$NativeOnly,
    [string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-05'),
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\d3dx9-version-probes')
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-d3dx9-versions.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\d3dx9-versions-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$prefix=Join-Path $suite 'prefix'
$loader=Join-Path $RuntimeDirectory 'wine64.exe'
$saved=$env:WINEDLLOVERRIDES
$cases=[Collections.Generic.List[object]]::new()
$nativePaths=@{}
function Probe-Name([int]$version,[int]$bits,[bool]$static){
    if($static){return "d3dx9-$version-import-probe$bits.exe"}
    return "d3dx9-$version-probe$bits.exe"
}
function Invoke-Probe([string]$label,[int]$version,[int]$bits,[bool]$static,[string]$expected,[string]$runtime){
    $name=Probe-Name $version $bits $static
    $log=Join-Path $suite "$label.log"
    if($runtime){
        & $runtime --prefix $prefix --desktop off run "C:\tests\$name" --expected-dll $expected *> $log
    }else{
        & (Join-Path $FixtureDirectory $name) --expected-dll $expected *> $log
    }
    $code=$LASTEXITCODE
    $cases.Add(@{fixture=$label;version=$version;bits=$bits;staticImport=$static;expectedProvider=$expected;exitCode=$code;status=$(if($code){'failed'}else{'passed'})})
    if($code){Get-Content -LiteralPath $log -Tail 30;throw "D3DX version regression failed: $label (exit $code)"}
    Write-Host "PASS $label"
}
try {
    # Native baseline independently establishes the tested SDK/ABI behavior.
    $env:WINEDLLOVERRIDES=$null
    foreach($version in @(37,41,42)){foreach($bits in @(32,64)){
        $native=Join-Path $env:WINDIR $(if($bits -eq 32){"SysWOW64\d3dx9_$version.dll"}else{"System32\d3dx9_$version.dll"})
        $nativePaths["$version-$bits"]=$native
        if(Test-Path -LiteralPath $native){
            foreach($static in @($false,$true)){Invoke-Probe "baseline-native-$version-$bits-static$static" $version $bits $static $native ''}
        }else{$cases.Add(@{fixture="baseline-native-$version-$bits";status='unavailable';reason='Optional native legacy SDK runtime is absent';path=$native})}
    }}
    if(!$NativeOnly){
        & $loader --prefix $prefix init *> (Join-Path $suite 'init.log')
        if($LASTEXITCODE){throw 'Versioned D3DX prefix initialization failed'}
        $programs=Join-Path $prefix 'drive_c\tests'
        New-Item -ItemType Directory -Path $programs|Out-Null
        Get-ChildItem -LiteralPath $FixtureDirectory -Filter 'd3dx9-*-probe*.exe'|ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $programs}
        foreach($version in @(37,41,42)){foreach($bits in @(32,64)){
            $arch=if($bits -eq 32){'x86'}else{'x64'}
            $native=$nativePaths["$version-$bits"]
            foreach($helper in @('builtin','native')){
                if($helper -eq 'native' -and !(Test-Path -LiteralPath $native)){continue}
                $expected=if($helper -eq 'builtin'){Join-Path $RuntimeDirectory "builtin\$arch\d3dx9_$version.dll"}else{$native}
                $helperOrder=if($helper -eq 'builtin'){'b,n'}else{'n,b'}
                foreach($graphics in @('builtin','native')){
                    $graphicsOrder=if($graphics -eq 'builtin'){'b,n'}else{'n,b'}
                    $env:WINEDLLOVERRIDES="d3dx9_$version=$helperOrder;d3d9=$graphicsOrder"
                    foreach($static in @($false,$true)){
                        Invoke-Probe "$helper-helper-$version-$graphics-d3d9-$bits-static$static" $version $bits $static $expected $loader
                    }
                }
            }
        }}
        # Reversible availability tests use a private runtime copy only.
        $fallback=Join-Path $suite 'runtime-without-versioned-helpers'
        New-Item -ItemType Directory -Path $fallback|Out-Null
        foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){
            Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback
        }
        foreach($arch in @('x86','x64')){
            $destination=Join-Path $fallback "builtin\$arch"
            New-Item -ItemType Directory -Path $destination|Out-Null
            Get-ChildItem -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch") -File |
                Where-Object {$_.Name -notin @('d3dx9_37.dll','d3dx9_41.dll','d3dx9_42.dll')} |
                ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination $destination}
        }
        $fallbackLoader=Join-Path $fallback 'wine64.exe'
        foreach($version in @(37,41,42)){foreach($bits in @(32,64)){
            $native=$nativePaths["$version-$bits"]
            if(Test-Path -LiteralPath $native){
                $env:WINEDLLOVERRIDES="d3dx9_$version=b,n;d3d9=n"
                Invoke-Probe "missing-builtin-native-fallback-$version-$bits" $version $bits $true $native $fallbackLoader
            }
            $env:WINEDLLOVERRIDES="d3dx9_$version=b;d3d9=n"
            $label="missing-strict-builtin-$version-$bits";$log=Join-Path $suite "$label.log"
            $name=Probe-Name $version $bits $false
            & $fallbackLoader --prefix $prefix run "C:\tests\$name" --expected-dll $native *> $log
            $code=$LASTEXITCODE;$contents=Get-Content -Raw -LiteralPath $log
            $rejected=$code -ne 0 -and $contents -match "No available implementation.*d3dx9_$version"
            $cases.Add(@{fixture=$label;exitCode=$code;status=$(if($rejected){'rejected-as-required'}else{'failed'})})
            if(!$rejected){throw "Strict missing D3DX builtin was not rejected: $label"}
            Write-Host "PASS $label"
        }}
    }
    @{passed=$true;nativeOnly=[bool]$NativeOnly;runtime=$RuntimeDirectory;cases=$cases;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "D3DX version suite passed ($($cases.Count) cases): $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}finally{$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
