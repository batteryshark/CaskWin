param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'dist\winedows-0.8-alpha'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\graphics-09\fixtures'),[ValidateSet(32,64)][int]$Bits=32,[ValidateSet('default','discrete')][string]$Preference='discrete',[switch]$Desktop,[switch]$SwapChain,[switch]$UnbindBeforeWorkers,[switch]$ForeignContext,[switch]$BackendContract,[string]$CasePattern='*',[string]$DebugChannels='err+all,warn+d3d,trace+d3d,+timestamp,+pid',[switch]$AllowKnownFailures)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$output=Join-Path $PSScriptRoot ('test-output\graphics-09\textures-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$cases=[Collections.Generic.List[object]]::new()
& $runtime --prefix $prefix init|Out-Host;if($LASTEXITCODE){throw 'Texture prefix init failed'}
$app=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $app|Out-Null
$file="d3d11-texture-$Preference$Bits.exe";Copy-Item -LiteralPath "$FixtureDirectory\$file" -Destination $app
Copy-Item -LiteralPath "$PSScriptRoot\test-output\debug-09\fixtures\gui-log-capture64.exe" -Destination $app
foreach($mode in @('native','wine-csmt0','wine-csmt1')){
    if($mode -notlike $CasePattern){continue}
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$runtime;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.Environment['WINEDLLOVERRIDES']=if($mode -eq 'native'){'dxgi,d3d11=n'}else{'dxgi,d3d11=b'}
    $start.Environment['WINE_D3D_CONFIG']=if($mode -eq 'wine-csmt0'){'renderer=gl,csmt=0'}else{'renderer=gl,csmt=1'}
    # Real adapter GL_RENDERER appears before the context/texture activity.
    # Deliberately omit d3d_sync: that trace channel forces serialization.
    $start.Environment['WINEDEBUG']=$DebugChannels
    $start.Environment['D3D11_THREAD_DUMP']="C:\tests\$mode.dmp"
    $arguments=@('--prefix',$prefix);if($Desktop){$arguments+=@('--desktop','Textures,800x600')}
    $arguments+=@('run','C:\tests\gui-log-capture64.exe',"C:\tests\$mode.log","C:\tests\$file");if($mode -ne 'native'){$arguments+='--wine'};if($SwapChain){$arguments+='--swapchain'};if($UnbindBeforeWorkers){$arguments+='--unbind-before-workers'};if($ForeignContext){$arguments+='--foreign-context'};if($BackendContract){$arguments+='--backend-contract'}
    foreach($arg in $arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$timeout=!$process.WaitForExit(60000)
    if($timeout){$process.Kill($true);$process.WaitForExit()};$code=$process.ExitCode;$outer=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose();$outer|Set-Content -LiteralPath "$output\$mode-launcher.log"
    $log=Join-Path $app "$mode.log";$text=if(Test-Path -LiteralPath $log){Get-Content -LiteralPath $log -Raw}else{''}
    $metadataPath="$log.status.json";$metadata=if(Test-Path -LiteralPath $metadataPath){Get-Content -LiteralPath $metadataPath -Raw|ConvertFrom-Json}else{$null}
    $passed=!$timeout -and $code -eq 0 -and $text -match 'SUMMARY failures=0'
    $summary=@($text -split "`r?`n"|Where-Object {$_ -match '(^BEGIN |^API |^QUERY |^ADAPTER |^OPENGL_DRIVER |^OBJECT_PROVIDER |^BACKEND_CONTRACT |^NATIVE_D3D11_IAT |^CRASH |^SUMMARY |^WGL_CURRENT |^DIAGNOSTIC_|GL_RENDERER:|GL_VENDOR:|GL_VERSION:|Failed|failed|wgl.*error)'})
    $summary|Set-Content -LiteralPath "$output\$mode-summary.log"
    $cases.Add(@{name=$mode;passed=$passed;exitCode=$code;timedOut=$timeout;log=$log;capture=$metadata;summary=$summary});Write-Host "$mode exit=$code passed=$passed"
}
$passed=$cases.Count -gt 0 -and !($cases|Where-Object {!$_.passed});@{passed=$passed;runtime=$RuntimeDirectory;bits=$Bits;preference=$Preference;desktop=[bool]$Desktop;swapchain=[bool]$SwapChain;diagnosticUnbind=[bool]$UnbindBeforeWorkers;foreignContext=[bool]$ForeignContext;backendContract=[bool]$BackendContract;cases=$cases;noPresentInputCaptureOrHostChanges=$true;limitations=@('Debug tracing changes timing','Captured logs are capped by gui-log-capture; inspect truncation metadata','Adapter description and loaded ICD are not by themselves proof of active GL renderer; use GL_RENDERER trace')}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath "$output\result.json"
Write-Host "Texture/thread evidence: $output";if(!$passed -and !$AllowKnownFailures){throw 'Texture/thread case failed; retained diagnostics'}
$global:LASTEXITCODE=0
