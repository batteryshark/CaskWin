param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-wine.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\audio-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Audio prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){
    foreach($kind in @('audio-probe','audio-import-probe')){
        $source=Join-Path "$PSScriptRoot\bin" "$kind$bits.exe"
        if(!(Test-Path -LiteralPath $source)){throw "Missing audio fixture: $source"}
        Copy-Item -LiteralPath $source -Destination (Join-Path $directory "$kind$bits.exe")
    }
}
$saved=$env:WINEDLLOVERRIDES
$checks=[System.Collections.Generic.List[object]]::new()
$hostHasEndpoint=@{}
$skipped=$false
try {
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        & "$PSScriptRoot\bin\audio-probe$bits.exe" --native 2>&1|Tee-Object -FilePath (Join-Path $prefix "host-$bits.log")
        $baseline=$LASTEXITCODE
        if($baseline -ne 0 -and $baseline -ne 77){throw "Native host audio baseline failed ($bits-bit)"}
        $hostHasEndpoint[$bits]=($baseline -eq 0)
        $checks.Add(@{fixture="host-$bits";exitCode=$baseline;status=$(if($baseline -eq 77){'no-render-endpoint'}else{'passed'})})
        if($baseline -eq 77){$skipped=$true}
        foreach($order in @('b,n','n,b')){
            $env:WINEDLLOVERRIDES="dsound=$order"
            foreach($kind in @('audio-probe','audio-import-probe')){
                $arguments=@('--prefix',$prefix,'run',"C:\tests\$kind$bits.exe")
                if($order -eq 'n,b'){$arguments+='--native'}else{$arguments+=@('--expected-dll',"$PSScriptRoot\bin\builtin\$arch\dsound.dll")}
                $label="$kind-$bits-$($order.Replace(',',''))"
                & $runtime @arguments 2>&1|Tee-Object -FilePath (Join-Path $prefix "$label.log")
                $code=$LASTEXITCODE
                $checks.Add(@{fixture=$label;exitCode=$code;status=$(if($code -eq 77){'no-render-endpoint'}elseif($code){'failed'}else{'passed'})})
                if($code -eq 77){
                    if($hostHasEndpoint[$bits]){throw "The prefix lost a render endpoint present in the native host baseline: $label"}
                    $skipped=$true;continue
                }
                if($code){throw "Audio fixture failed: $label; inspect $prefix"}
            }
        }
        $env:WINEDLLOVERRIDES='dsound=b'
        & $runtime --prefix $prefix --desktop 'Audio,800x600' run "C:\tests\audio-probe$bits.exe" --expected-dll "$PSScriptRoot\bin\builtin\$arch\dsound.dll" 2>&1|Tee-Object -FilePath (Join-Path $prefix "desktop-$bits.log")
        $code=$LASTEXITCODE
        $checks.Add(@{fixture="desktop-$bits";exitCode=$code;status=$(if($code -eq 77){'no-render-endpoint'}elseif($code){'failed'}else{'passed'})})
        if($code -eq 77){
            if($hostHasEndpoint[$bits]){throw "The virtual desktop lost a render endpoint present in the native host baseline ($bits-bit)"}
            $skipped=$true
        }elseif($code){throw "Audio in virtual desktop failed ($bits-bit)"}
    }
    @{passed=(!$skipped);status=$(if($skipped){'incomplete-no-render-endpoint'}else{'passed'});silentPlayback=$true;capturePerformed=$false;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    if($skipped){Write-Host "Audio coverage incomplete: no render endpoint in one or more runs. Artifacts: $prefix";$global:LASTEXITCODE=77}
    else{Write-Host "Wine/native audio suite passed: $prefix";$global:LASTEXITCODE=0}
} catch {
    @{passed=$false;status='failed';error=$_.Exception.Message;silentPlayback=$true;capturePerformed=$false;checks=$checks;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    throw
} finally {$env:WINEDLLOVERRIDES=$saved}
