param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$FixtureDirectory,
    [string]$MediaFile
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\reader-av-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix'
$results=[Collections.Generic.List[object]]::new()
$loader=Join-Path $RuntimeDirectory 'wine64.exe'
$saved=@{overrides=$env:WINEDLLOVERRIDES;config=$env:WINE_D3D_CONFIG;trace=$env:WINEDOWS_TRACE;debug=$env:WINEDEBUG}
New-Item -ItemType Directory -Path $suite|Out-Null
function Run-ReaderCase([string]$label,[string]$exe,[string[]]$arguments,[string[]]$required){
    $quoted=@($arguments|ForEach-Object{'"'+([regex]::Replace(([regex]::Replace($_,'(\\*)"','$1$1\"')),'(\\+)$','$1$1'))+'"'})
    $log=Join-Path $suite "$label.log"
    $p=Start-Process -FilePath $exe -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError (Join-Path $suite "$label.stderr.log")
    $timeout=!$p.WaitForExit(45000)
    if($timeout){$p.Kill($true);$p.WaitForExit()}
    $code=if($timeout){124}else{$p.ExitCode}
    $content=Get-Content -LiteralPath $log -Raw
    $missing=@($required|Where-Object{$content -notmatch $_})
    $results.Add(@{case=$label;exit=$code;timeout=$timeout;missing=$missing;queuedBatches=([regex]::Matches($content,'PASS all outstanding requests complete exactly once')).Count})
    $results|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'cases.json')
    if($code -or $missing.Count){throw "Reader regression failed: $label (exit $code, missing $missing)"}
    Write-Host "PASS $label"
}
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINE_D3D_CONFIG=$null;$env:WINEDOWS_TRACE=$null;$env:WINEDEBUG=$null
    & $loader --prefix $prefix init *> (Join-Path $suite 'init.log')
    if($LASTEXITCODE){throw 'Cannot initialize reader regression prefix'}
    $programs=Join-Path $prefix 'drive_c\tests'
    New-Item -ItemType Directory -Path $programs|Out-Null
    foreach($bits in @(32,64)){
        foreach($kind in @('av','video')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "$kind-probe$bits.exe") -Destination $programs}
    }
    Get-ChildItem -LiteralPath $programs -Filter '*.exe'|Get-FileHash|Select-Object Path,Hash|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $suite 'fixture-hashes.json')
    if($MediaFile){Copy-Item -LiteralPath $MediaFile -Destination (Join-Path $programs 'generated-av.mp4')}
    else {Run-ReaderCase 'generate-av' (Join-Path $programs 'av-probe64.exe') @('--generate',(Join-Path $programs 'generated-av.mp4')) @()}
    Run-ReaderCase 'generate-video' (Join-Path $programs 'video-probe64.exe') @('--generate',(Join-Path $programs 'generated.mp4')) @()
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        foreach($provider in @('n','b')){
            $env:WINEDLLOVERRIDES="mfreadwrite=$provider;dxgi,d3d11=n"
            $identity=if($provider -eq 'b'){[regex]::Escape((Join-Path $RuntimeDirectory "builtin\$arch\mfreadwrite.dll"))}else{'(?i)\\Windows\\(System32|SysWOW64)\\mfreadwrite.dll'}
            foreach($mode in @('lifecycle','async-lifecycle','drain','async-drain','async-queued','async-selection')){
                Run-ReaderCase "$provider-$bits-$mode" $loader @('--prefix',$prefix,'run',"C:\tests\av-probe$bits.exe","--$mode",'C:\tests\generated-av.mp4') @($identity,'real native DXVA submissions accompany converted video','both streams reach EOF','AAC decoded channel preserves generated tone and level')
            }
            $videoMode=if($provider -eq 'b'){'--async-lifecycle-wine'}else{'--async-lifecycle'}
            Run-ReaderCase "$provider-$bits-video-lifecycle" $loader @('--prefix',$prefix,'--desktop','Reader regression,800x600','run',"C:\tests\video-probe$bits.exe",$videoMode,'C:\tests\generated.mp4') @($identity,'successful native DXVA decode submissions prove hardware use')
        }
        $env:WINEDLLOVERRIDES='mfreadwrite=b;dxgi,d3d11=n'
        foreach($round in 2..3){Run-ReaderCase "b-$bits-async-queued-$round" $loader @('--prefix',$prefix,'run',"C:\tests\av-probe$bits.exe",'--async-queued','C:\tests\generated-av.mp4') @('PASS all outstanding requests complete exactly once','real native DXVA submissions accompany converted video','AAC decoded channel preserves generated tone and level')}
    }
    @{passed=$true;cases=$results;scope='Unmodified audiovisual/video fixture binaries, native and Wine reader lifecycle/drain/queued/selection paths, original caller-supplied MP4 when provided; no audio playback or capture.'}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Reader AV regression passed: $suite"
}catch{@{passed=$false;cases=$results;error=$_.Exception.Message}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw}
finally{$env:WINEDLLOVERRIDES=$saved.overrides;$env:WINE_D3D_CONFIG=$saved.config;$env:WINEDOWS_TRACE=$saved.trace;$env:WINEDEBUG=$saved.debug}
$global:LASTEXITCODE=0
