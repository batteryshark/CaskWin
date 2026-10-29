param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-system-loader-probe.ps1" -OutputDirectory $RuntimeDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\system-loader-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
$saved=$env:WINEDLLOVERRIDES;$results=@();$failures=@()
function Invoke-LoaderProbe([string]$label,[string]$loader,[string[]]$arguments,[string]$expectedError=''){
    $output=& $loader @arguments 2>&1;$status=$LASTEXITCODE
    $output|Tee-Object -FilePath (Join-Path $suite "$label.log")|Write-Host
    $passed=if($expectedError){$status -ne 0 -and ($output -join "`n") -match $expectedError}else{$status -eq 0}
    $script:results+=@{case=$label;passed=$passed;exit=$status}
    if(!$passed){$script:failures+=$label}
}
try{
    $env:WINEDLLOVERRIDES=$null
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'System loader test prefix initialization failed'}
    foreach($bits in @(32,64)){
        foreach($kind in @('clean','local')){
            $directory=Join-Path $prefix "drive_c\$kind$bits"
            New-Item -ItemType Directory -Force -Path $directory|Out-Null
            Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "system-loader-probe$bits.exe") -Destination (Join-Path $directory 'probe.exe')
        }
        $directory=Join-Path $prefix "drive_c\local$bits"
        Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "system-loader-marker$bits.dll") -Destination (Join-Path $directory 'xinput1_4.dll')
        New-Item -ItemType File -Path (Join-Path $directory 'probe.exe.local')|Out-Null
        Invoke-LoaderProbe "system-identity-accepted-$bits" $runtime @('--prefix',$prefix,'run',"C:\clean$bits\probe.exe",'--accept')
        $output=& (Join-Path $directory 'probe.exe') --raw-local 2>&1;$status=$LASTEXITCODE
        $output|Tee-Object -FilePath (Join-Path $suite "native-dotlocal-$bits.log")|Write-Host
        if($status -eq 77){
            $results+=@{case="dotlocal-$bits";passed=$null;status='unavailable';reason='Native Windows ignored DotLocal in this process; no host policy altered'}
        }elseif($status){$failures+="native-dotlocal-$bits"}
        else{
            Invoke-LoaderProbe "dotlocal-rejected-$bits" $runtime @('--prefix',$prefix,'run',"C:\local$bits\probe.exe",'--reject-local')
        }
    }
    # All corrupt/wrong-architecture images belong to this disposable clone.
    $clone=Join-Path $suite 'runtime-preflight'
    New-Item -ItemType Directory -Path $clone|Out-Null
    foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $clone}
    $family=@('wined3d','dxgi','d3d11','d3d10core','d3d10_1','d3d10','d3dcompiler_39')
    foreach($arch in @('x86','x64')){
        $destination=Join-Path $clone "builtin\$arch";New-Item -ItemType Directory -Force -Path $destination|Out-Null
        foreach($name in $family){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch\$name.dll") -Destination $destination}
    }
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$other=if($bits -eq 32){'x64'}else{'x86'}
        $file=Join-Path $clone "builtin\$arch\d3d10.dll"
        foreach($kind in @('wrong-architecture','malformed')){
            if($kind -eq 'wrong-architecture'){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "builtin\$other\d3d10.dll") -Destination $file}
            else{'This disposable test image is deliberately not a PE DLL.'|Set-Content -LiteralPath $file}
            $env:WINEDLLOVERRIDES='d3d11=b,n'
            Invoke-LoaderProbe "$kind-native-fallback-$bits" (Join-Path $clone 'wine64.exe') @('--prefix',$prefix,'run',"C:\clean$bits\probe.exe",'--family-native')
            $env:WINEDLLOVERRIDES='d3d11=b'
            $expected=if($kind -eq 'wrong-architecture'){'Wine builtin architecture does not match the program: .*d3d10\.dll'}else{'Cannot use Wine builtin .*d3d10\.dll: Invalid'}
            Invoke-LoaderProbe "$kind-strict-rejection-$bits" (Join-Path $clone 'wine64.exe') @('--prefix',$prefix,'run',"C:\clean$bits\probe.exe",'--family-native') $expected
        }
        Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch\d3d10.dll") -Destination $file
    }
}finally{
    $env:WINEDLLOVERRIDES=$saved
    @{passed=(!$failures.Count -and $results.Count -ge 12);cases=$results;failures=$failures;hostPolicyChanged=$false;
      limitations=@('DotLocal rejection covers module identity after loading, not DLL initialization side effects or a security boundary','Native DLL initialization failure retry is not implemented');
      utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "System loader results: $suite"
}
if($failures.Count){throw "System loader regressions: $($failures -join ', ')"}
$global:LASTEXITCODE=0
