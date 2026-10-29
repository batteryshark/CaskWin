param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-08'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\xact-08\fixtures'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-xact-probe.ps1" -OutputDirectory $FixtureDirectory -WineSourceDirectory $WineSourceDirectory}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$FixtureDirectory=(Resolve-Path -LiteralPath $FixtureDirectory).Path
$prefix=Join-Path $PSScriptRoot ('test-output\xact-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$saved=$env:WINEDLLOVERRIDES;$checks=[System.Collections.Generic.List[object]]::new()
$limitations=@('XACT 3.7 only; earlier CLSIDs and interface versions are not substituted.','Muted tests verify clocks, callbacks, decoded-source completion and streaming; they do not establish audible game quality.','Upstream FACT xWMA duration metadata differs from native; tests verify actual timed completion.','The upstream Wine wrapper ignores supplied shared XAudio/mastering-voice objects; effect DSP, simple-wave cue EOF notifications and untested notification/filtering behavior retain upstream limits.','Blossom Tales II uses FNA direct FACT calls, so installing this COM provider does not redirect its existing FAudio path.')
function Invoke-XactCase([string]$Label,[string]$Executable,[string[]]$Arguments){
    $log=Join-Path $prefix "$Label.log";$err=Join-Path $prefix "$Label.stderr.log"
    $quoted=@($Arguments|ForEach-Object{'"'+([regex]::Replace(([regex]::Replace($_,'(\\*)"','$1$1\"')),'(\\+)$','$1$1'))+'"'})
    $process=Start-Process -FilePath $Executable -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $timeout=!$process.WaitForExit(35000);if($timeout){$process.Kill($true);$process.WaitForExit()}
    $code=if($timeout){124}else{$process.ExitCode};$checks.Add(@{fixture=$Label;exitCode=$code;timedOut=$timeout})
    Write-Host "$Label : exit $code"
    if($code){Get-Content -LiteralPath $log -Tail 24;Get-Content -LiteralPath $err -Tail 12}
}
try{
    $env:WINEDLLOVERRIDES=$null;& $runtime --prefix $prefix init;if($LASTEXITCODE){throw 'XACT prefix initialization failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($file in @('xact-probe32.exe','xact-probe64.exe','test.xwb','streaming.xwb')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory $file) -Destination $directory}
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$system=if($bits -eq 32){'SysWOW64'}else{'System32'}
        $native=Join-Path $env:WINDIR "$system\xactengine3_7.dll";$builtin=Join-Path $RuntimeDirectory "builtin\$arch\xactengine3_7.dll"
        foreach($kind in @('memory','streaming')){
            $bank=if($kind -eq 'streaming'){'streaming.xwb'}else{'test.xwb'}
            $extra=@();if($kind -eq 'streaming'){$extra+= '--streaming'}
            if(Test-Path -LiteralPath $native){$env:WINEDLLOVERRIDES=$null;Invoke-XactCase "host-$bits-$kind" "$FixtureDirectory\xact-probe$bits.exe" (@($native,"$FixtureDirectory\$bank")+$extra)}
            else{$checks.Add(@{fixture="host-$bits-$kind";status='native-provider-not-installed'})}
            foreach($policy in @('b','n','b,n','n,b')){
                if($policy -eq 'n' -and !(Test-Path -LiteralPath $native)){continue}
                $expected=if($policy.StartsWith('n') -and (Test-Path -LiteralPath $native)){$native}else{$builtin}
                $env:WINEDLLOVERRIDES="xactengine3_7=$policy"
                $activation=if($kind -eq 'streaming'){'--com-ex'}else{'--com'}
                Invoke-XactCase "$policy-$bits-$kind" $runtime (@('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$expected,"C:\tests\$bank",$activation)+$extra)
            }
        }
        foreach($activation in @('--com','--com-ex')){
            foreach($registration in @('--private-empty','--private-missing')){
                $env:WINEDLLOVERRIDES='xactengine3_7=n'
                Invoke-XactCase "native-$bits-$activation-$registration" $runtime @('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$native,'C:\tests\test.xwb',$activation,$registration,'--expect-activation-failure')
                $env:WINEDLLOVERRIDES='xactengine3_7=b'
                Invoke-XactCase "wine-$bits-$activation-$registration" $runtime @('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$builtin,'C:\tests\test.xwb',$activation,$registration)
            }
        }
        if(Test-Path -LiteralPath $native){
            $env:WINEDLLOVERRIDES='xactengine3_7=n'
            Invoke-XactCase "native-$bits-explicit-registration" $runtime @('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$native,'C:\tests\test.xwb','--com','--private-native')
        }
        $env:WINEDLLOVERRIDES='xactengine3_7=b'
        Invoke-XactCase "wine-$bits-direct-factory" $runtime @('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$builtin,'C:\tests\test.xwb')
    }
    $failed=@($checks|Where-Object {$_.ContainsKey('exitCode') -and $_.exitCode -ne 0})
    if($failed.Count){throw "XACT fixtures failed: $($failed.fixture -join ', '); inspect $prefix"}
    @{passed=$true;checks=$checks;limitations=$limitations;silentPlayback=$true;capturePerformed=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "XACT native/Wine suite passed: $prefix";$global:LASTEXITCODE=0
}catch{
    if(Test-Path -LiteralPath $prefix){@{passed=$false;error=$_.Exception.Message;checks=$checks;limitations=$limitations}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $prefix 'result.json')};throw
}finally{$env:WINEDLLOVERRIDES=$saved}
