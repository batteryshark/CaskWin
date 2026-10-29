param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\xact-08\runtime-final'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\xact-08\fixtures'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-xact-probe.ps1" -OutputDirectory $FixtureDirectory}
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path;$FixtureDirectory=(Resolve-Path -LiteralPath $FixtureDirectory).Path
$prefix=Join-Path $PSScriptRoot ('test-output\xact-managed-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$saved=$env:WINEDLLOVERRIDES;$checks=[System.Collections.Generic.List[object]]::new()
function Invoke-XactManaged([string]$Label,[string]$Executable,[string[]]$Arguments){
    $log=Join-Path $prefix "$Label.log";$err=Join-Path $prefix "$Label.stderr.log"
    $quoted=@($Arguments|ForEach-Object{'"'+([regex]::Replace(([regex]::Replace($_,'(\\*)"','$1$1\"')),'(\\+)$','$1$1'))+'"'})
    $process=Start-Process -FilePath $Executable -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $timeout=!$process.WaitForExit(35000);if($timeout){$process.Kill($true);$process.WaitForExit()}
    $code=if($timeout){124}else{$process.ExitCode};$checks.Add(@{fixture=$Label;exitCode=$code;timedOut=$timeout});Write-Host "$Label : exit $code"
    if($code){Get-Content -LiteralPath $log -Tail 25;Get-Content -LiteralPath $err -Tail 10}
}
try{
    $env:WINEDLLOVERRIDES=$null;& $runtime --prefix $prefix init;if($LASTEXITCODE){throw 'Managed-wave prefix initialization failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($file in @('xact-probe32.exe','xact-probe64.exe','silent-play.xwb','silent-streaming.xwb','test.xwb','streaming.xwb')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory $file) -Destination $directory}
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$system=if($bits -eq 32){'SysWOW64'}else{'System32'}
        $native=Join-Path $env:WINDIR "$system\xactengine3_7.dll";$builtin=Join-Path $RuntimeDirectory "builtin\$arch\xactengine3_7.dll"
        foreach($kind in @('memory','streaming')){
            $bank=if($kind -eq 'streaming'){'silent-streaming.xwb'}else{'silent-play.xwb'}
            foreach($subscription in @('persistent','none','unregistered','object','object-unregistered')){
                $extra=@('--no-handle-only');if($kind -eq 'streaming'){$extra+='--streaming'}
                if($subscription -eq 'none'){$extra+='--no-destroy-notification'}
                if($subscription -eq 'unregistered'){$extra+='--unregister-destroy'}
                if($subscription -eq 'object'){$extra+='--object-destroy'}
                if($subscription -eq 'object-unregistered'){$extra+='--unregister-object-destroy'}
                if(Test-Path -LiteralPath $native){$env:WINEDLLOVERRIDES=$null;Invoke-XactManaged "host-$bits-$kind-$subscription" "$FixtureDirectory\xact-probe$bits.exe" (@($native,"$FixtureDirectory\$bank")+$extra)}
                else{$checks.Add(@{fixture="host-$bits-$kind-$subscription";status='native-provider-not-installed'})}
                $env:WINEDLLOVERRIDES='xactengine3_7=b'
                Invoke-XactManaged "wine-$bits-$kind-$subscription" $runtime (@('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$builtin,"C:\tests\$bank",'--com')+$extra)
            }
            # Recheck caller-owned wave/cue paths after managed lifetime changes.
            $bank=if($kind -eq 'streaming'){'streaming.xwb'}else{'test.xwb'};$extra=@();if($kind -eq 'streaming'){$extra+='--streaming'}
            $env:WINEDLLOVERRIDES='xactengine3_7=b'
            Invoke-XactManaged "wine-$bits-$kind-owned-regression" $runtime (@('--prefix',$prefix,'run',"C:\tests\xact-probe$bits.exe",$builtin,"C:\tests\$bank",'--com-ex')+$extra)
        }
    }
    $failed=@($checks|Where-Object {$_.ContainsKey('exitCode') -and $_.exitCode -ne 0});if($failed.Count){throw "Managed XACT fixtures failed: $($failed.fixture -join ', '); inspect $prefix"}
    @{passed=$true;checks=$checks;silentPCMOnlyForUnmutedCalls=$true;capturePerformed=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "Managed XACT suite passed: $prefix";$global:LASTEXITCODE=0
}catch{if(Test-Path -LiteralPath $prefix){@{passed=$false;error=$_.Exception.Message;checks=$checks}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')};throw}finally{$env:WINEDLLOVERRIDES=$saved}
