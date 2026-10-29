param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[switch]$RequireWicCom)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-imaging-probes.ps1"}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$suite=Join-Path $PSScriptRoot ('test-output\imaging-'+[guid]::NewGuid().ToString('N'))
$images=Join-Path $suite 'images';New-Item -ItemType Directory -Path $images|Out-Null
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$savedOverrides=$env:WINEDLLOVERRIDES;$savedTrace=$env:WINEDOWS_TRACE
$checks=[Collections.Generic.List[object]]::new()
function Invoke-Fixture([string]$label,[string]$program,[string[]]$arguments,[bool]$required=$true){
    $quoted=@($arguments|ForEach-Object {'"'+([regex]::Replace(([regex]::Replace($_,'(\\*)"','$1$1\"')),'(\\+)$','$1$1'))+'"'})
    $log=Join-Path $suite "$label.log";$errorLog=Join-Path $suite "$label.stderr.log"
    $process=Start-Process -FilePath $program -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errorLog
    $timeout=!$process.WaitForExit(60000);if($timeout){$process.Kill($true);$process.WaitForExit()}
    $code=if($timeout){124}else{$process.ExitCode};$checks.Add(@{fixture=$label;exitCode=$code;required=$required;timedOut=$timeout})
    Write-Host "$label : exit $code"
    if($code){Get-Content -LiteralPath $log -Tail 30|ForEach-Object {Write-Host $_};Get-Content -LiteralPath $errorLog -Tail 12|ForEach-Object {Write-Host $_}}
    return $code
}
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_TRACE=$null
    if(Invoke-Fixture 'generate' "$PSScriptRoot\bin\d3dx-image-probe64.exe" @('--generate',$images)){throw 'Native fixture generation failed'}
    $hashesBefore=@(Get-ChildItem -LiteralPath $images -File|Sort-Object Name|Get-FileHash -Algorithm SHA256|Select-Object -ExpandProperty Hash)
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'Imaging prefix initialization failed'}
    $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
    Copy-Item -LiteralPath $images -Destination $tests -Recurse
    foreach($bits in @(32,64)){
        foreach($name in @('d3dx-image-probe','wic-probe')){Copy-Item -LiteralPath "$PSScriptRoot\bin\$name$bits.exe" -Destination $tests}
        $arch=if($bits -eq 32){'x86'}else{'x64'};$system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
        $nativeHelper=Join-Path $system 'd3dx9_43.dll';$nativeD3d=Join-Path $system 'd3d9.dll'
        $env:WINEDLLOVERRIDES=$null
        $null=Invoke-Fixture "host-wic-$bits" "$PSScriptRoot\bin\wic-probe$bits.exe" @('--images',$images)
        if(Test-Path -LiteralPath $nativeHelper){$null=Invoke-Fixture "host-native-textures-$bits" "$PSScriptRoot\bin\d3dx-image-probe$bits.exe" @('--test',$images,$nativeHelper,$nativeD3d)}
        $env:WINEDOWS_TRACE='1'
        $null=Invoke-Fixture "prefix-wic-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\wic-probe$bits.exe",'--images','C:\tests\images') ([bool]$RequireWicCom)
        foreach($helper in @('wine','native')){
            if($helper -eq 'native' -and !(Test-Path -LiteralPath $nativeHelper)){continue}
            foreach($graphics in @('wine','native')){
                $helperChoice=if($helper -eq 'wine'){'b'}else{'n'};$graphicsChoice=if($graphics -eq 'wine'){'b'}else{'n'}
                $env:WINEDLLOVERRIDES="d3dx9_43=$helperChoice;d3d9=$graphicsChoice"
                $expectedHelper=if($helper -eq 'wine'){Join-Path "$RuntimeDirectory\builtin\$arch" 'd3dx9_43.dll'}else{$nativeHelper}
                $expectedD3d=if($graphics -eq 'wine'){Join-Path "$RuntimeDirectory\builtin\$arch" 'd3d9.dll'}else{$nativeD3d}
                $null=Invoke-Fixture "$helper-helper-$graphics-d3d9-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\d3dx-image-probe$bits.exe",'--test','C:\tests\images',$expectedHelper,$expectedD3d)
            }
        }
    }
    $hashesAfter=@(Get-ChildItem -LiteralPath $images -File|Sort-Object Name|Get-FileHash -Algorithm SHA256|Select-Object -ExpandProperty Hash)
    if(Compare-Object $hashesBefore $hashesAfter){throw 'Generated source fixtures changed during decode tests'}
    $failed=@($checks|Where-Object {$_.required -and $_.exitCode -ne 0})
    if($failed.Count){throw "Imaging fixtures failed: $($failed.fixture -join ', ')"}
    $wicAvailable=@($checks|Where-Object {$_.fixture -like 'prefix-wic-*' -and $_.exitCode -ne 0}).Count -eq 0
    @{passed=$true;wicComAvailable=$wicAvailable;sourceFixturesUnchanged=$true;pixelChecks=@{png='exact RGBA';wineJpeg='RGB tolerance 2/255, exact alpha';nativeJpeg='RGB tolerance 16/255, exact alpha (legacy decoder differs on host too)';gpuReadback='exact RGBA versus decoded texture'};checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Imaging suite passed: $suite; public prefix WIC COM available=$wicAvailable"
    $global:LASTEXITCODE=0
}catch{
    @{passed=$false;error=$_.Exception.Message;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}finally{$env:WINEDLLOVERRIDES=$savedOverrides;$env:WINEDOWS_TRACE=$savedTrace}
