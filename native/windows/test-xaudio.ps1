param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-xaudio-probe.ps1" -WineSourceDirectory $WineSourceDirectory}
if(!(Test-Path -LiteralPath "$PSScriptRoot\bin\registry-com32.dll") -or !(Test-Path -LiteralPath "$PSScriptRoot\bin\registry-com64.dll")){
    if($SkipBuild){throw 'Missing registry COM fixture DLLs; run build-registry-probe.ps1 first.'}
    & "$PSScriptRoot\build-registry-probe.ps1"
}
$prefix=Join-Path $PSScriptRoot ('test-output\xaudio-'+[Guid]::NewGuid().ToString('N'))
$RuntimeDirectory=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'XAudio prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){
    Copy-Item -LiteralPath "$PSScriptRoot\bin\registry-com$bits.dll" -Destination $directory
    foreach($version in @(7,8,9)){
        $kinds=if($version -eq 7){@('probe')}else{@('probe','import-probe')}
        foreach($kind in $kinds){Copy-Item -LiteralPath "$PSScriptRoot\bin\xaudio$version-$kind$bits.exe" -Destination $directory}
    }
}
$saved=$env:WINEDLLOVERRIDES
$checks=[System.Collections.Generic.List[object]]::new()
$limitations=@('Bundled FAPOFX echo/EQ/reverb/limiter enabled DSP remains unimplemented upstream; helpers cover activation, frame queries and disabled in-place bypass only.','Silent streaming does not prove game-specific audible quality or long-play stability.')
function Invoke-Case([string]$Label,[string]$Executable,[string[]]$Arguments){
    $log=Join-Path $prefix "$Label.log";$err=Join-Path $prefix "$Label.stderr.log"
    # Use Windows argv escaping rather than PowerShell or a second shell.
    $quoted=@($Arguments|ForEach-Object{'"'+([regex]::Replace(([regex]::Replace($_,'(\\*)"','$1$1\"')),'(\\+)$','$1$1'))+'"'})
    $process=Start-Process -FilePath $Executable -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    $timeout=!$process.WaitForExit(45000)
    if($timeout){$process.Kill($true);$process.WaitForExit()}
    $code=if($timeout){124}else{$process.ExitCode}
    $checks.Add(@{fixture=$Label;exitCode=$code;timedOut=$timeout})
    Write-Host "$Label : exit $code"
    if($code){Get-Content -LiteralPath $log -Tail 45;Get-Content -LiteralPath $err -Tail 15}
}
try {
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $builtin=Join-Path "$RuntimeDirectory\builtin" $arch
        $system=if($bits -eq 32){Join-Path $env:WINDIR 'SysWOW64'}else{Join-Path $env:WINDIR 'System32'}
        foreach($version in @(7,8,9)){
            $dll="xaudio2_$version.dll"
            if(Test-Path -LiteralPath (Join-Path $system $dll)){
                $env:WINEDLLOVERRIDES=$null
                Invoke-Case "host-$version-$bits" "$PSScriptRoot\bin\xaudio$version-probe$bits.exe" @($dll,'native')
            }else{
                $checks.Add(@{fixture="host-$version-$bits";status='native-provider-not-installed'})
            }
            $kinds=if($version -eq 7){@('probe')}else{@('probe','import-probe')}
            foreach($kind in $kinds){
                $env:WINEDLLOVERRIDES="xaudio2_$version,x3daudio1_7,xapofx1_5=b"
                Invoke-Case "wine-$version-$kind-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio$version-$kind$bits.exe",$dll,(Join-Path $builtin $dll),$builtin)
                if(Test-Path -LiteralPath (Join-Path $system $dll)){
                    $env:WINEDLLOVERRIDES="xaudio2_$version=n"
                    Invoke-Case "native-$version-$kind-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio$version-$kind$bits.exe",$dll,'native')
                }
            }
        }
        $env:WINEDLLOVERRIDES='xaudio2_7=b'
        Invoke-Case "com-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll',(Join-Path $builtin 'xaudio2_7.dll'),'-','com')
        Invoke-Case "registered-wine-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll',(Join-Path $builtin 'xaudio2_7.dll'),'-','registered',"C:\tests\registry-com$bits.dll")
        if(Test-Path -LiteralPath (Join-Path $system 'xaudio2_7.dll')){
            $env:WINEDLLOVERRIDES='xaudio2_7=n'
            Invoke-Case "com-native-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll','native','-','com')
            Invoke-Case "registered-native-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll','native','-','registered',"C:\tests\registry-com$bits.dll")
            Invoke-Case "broken-registration-native-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll','native','-','registered-missing',"C:\tests\missing-com$bits.dll")
            Invoke-Case "empty-registration-native-7-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio7-probe$bits.exe",'xaudio2_7.dll','native','-','registered-empty',"C:\tests\missing-com$bits.dll")
        }
        $env:WINEDLLOVERRIDES='xaudio2_9=b'
        Invoke-Case "desktop-9-$bits" $runtime @('--prefix',$prefix,'--desktop','XAudio,800x600','run',"C:\tests\xaudio9-probe$bits.exe",'xaudio2_9.dll',(Join-Path $builtin 'xaudio2_9.dll'))
        $env:WINEDLLOVERRIDES='xaudio2_9=b,n'
        Invoke-Case "builtin-first-9-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio9-import-probe$bits.exe",'xaudio2_9.dll',(Join-Path $builtin 'xaudio2_9.dll'))
        $env:WINEDLLOVERRIDES='xaudio2_9=n,b'
        Invoke-Case "native-first-9-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio9-import-probe$bits.exe",'xaudio2_9.dll','native')
        if((Test-Path -LiteralPath (Join-Path $system 'x3daudio1_7.dll')) -and (Test-Path -LiteralPath (Join-Path $system 'xapofx1_5.dll'))){
            $env:WINEDLLOVERRIDES='xaudio2_9=b;x3daudio1_7,xapofx1_5=n'
            Invoke-Case "mixed-native-helpers-9-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests\xaudio9-probe$bits.exe",'xaudio2_9.dll',(Join-Path $builtin 'xaudio2_9.dll'),'native')
        }else{$checks.Add(@{fixture="mixed-native-helpers-9-$bits";status='native-helpers-not-installed'})}
    }
    $failed=@($checks | Where-Object {$_.ContainsKey('exitCode') -and $_.exitCode -ne 0})
    if($failed.Count){throw "XAudio fixtures failed: $($failed.fixture -join ', '); inspect $prefix"}
    @{passed=$true;checks=$checks;limitations=$limitations;silentPlayback=$true;capturePerformed=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "XAudio Wine/native suite passed: $prefix"
    $global:LASTEXITCODE=0
}catch{
    @{passed=$false;error=$_.Exception.Message;checks=$checks;limitations=$limitations;silentPlayback=$true;capturePerformed=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    throw
}finally{$env:WINEDLLOVERRIDES=$saved}
