param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'),[switch]$NativeOnly)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-registry-transfer-probe.ps1" -OutputDirectory $FixtureDirectory}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$suite=Join-Path $PSScriptRoot ('test-output\registry-transfer-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$failed=[Collections.Generic.List[string]]::new();$token='WinedowsTransfer-'+[Guid]::NewGuid().ToString('N')
function Invoke-Case([string]$label,[string]$program,[string[]]$arguments){
 & $program @arguments *> (Join-Path $suite "$label.log")
 if($LASTEXITCODE){$failed.Add("$label (exit $LASTEXITCODE)");Write-Host "FAIL $label"}else{Write-Host "PASS $label"}
}
foreach($p in @(32,64)){foreach($c in @(32,64)){
 Invoke-Case "native-$p-to-$c" (Join-Path $FixtureDirectory "registry-transfer-probe$p.exe") @('--transfers',(Join-Path $FixtureDirectory "registry-transfer-probe$c.exe"),'host')
}}
if(!$NativeOnly){
 $prefix=Join-Path $suite 'primary';$sibling=Join-Path $suite 'sibling'
 foreach($p in @($prefix,$sibling)){
  & $runtime --prefix $p init
  if($LASTEXITCODE){throw 'Registry transfer prefix initialization failed'}
  $directory=Join-Path $p 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
  foreach($bits in @(32,64)){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "registry-transfer-probe$bits.exe") -Destination $directory}
 }
 foreach($bits in @(32,64)){Invoke-Case "host-before-$bits" (Join-Path $FixtureDirectory "registry-transfer-probe$bits.exe") @('--absent',$token)}
 Invoke-Case 'seed' $runtime @('--prefix',$prefix,'run','C:\tests\registry-transfer-probe64.exe','--seed',$token)
 foreach($p in @(32,64)){foreach($c in @(32,64)){
  Invoke-Case "private-$p-to-$c" $runtime @('--prefix',$prefix,'run',"C:\tests\registry-transfer-probe$p.exe",'--transfers',"C:\tests\registry-transfer-probe$c.exe",$token)
  $nativeErrors=@(Get-Content -LiteralPath (Join-Path $suite "native-$p-to-$c.log")|Where-Object{$_ -like 'DUPLICATE-FAILURE *'})
  $privateErrors=@(Get-Content -LiteralPath (Join-Path $suite "private-$p-to-$c.log")|Where-Object{$_ -like 'DUPLICATE-FAILURE *'})
  if($nativeErrors.Count -ne 8 -or ($nativeErrors -join "`n") -ne ($privateErrors -join "`n")){$failed.Add("native/private duplication failure status mismatch $p-to-$c")}
 }}
 foreach($bits in @(32,64)){
  Invoke-Case "sibling-$bits" $runtime @('--prefix',$sibling,'run',"C:\tests\registry-transfer-probe$bits.exe",'--absent',$token)
  Invoke-Case "host-after-$bits" (Join-Path $FixtureDirectory "registry-transfer-probe$bits.exe") @('--absent',$token)
 }
}
@{passed=($failed.Count -eq 0);nativeOnly=[bool]$NativeOnly;token=$token;runtime=$RuntimeDirectory;
 coverage=@('all four x86/x64 parent-child combinations','native read-only registry baseline','explicit and implicit shared Classes handle views','handle inheritance','local-to-remote, remote-to-local and brokered remote-to-remote duplication','reduced access without KEY_QUERY_VALUE','success/failure with close-source','protected sources','observed remote-close handle reuse with opposite-view data','private child creation through inherited handle','unchanged opposite view, host and sibling');
 limitations=@('native key tag bits measured on this Windows host; other Windows versions require validation','explicit transfer is an ordinary native handle capability, not a security boundary');
 failures=@($failed);utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $suite 'result.json')
Write-Host "Registry transfer evidence: $suite"
if($failed.Count){throw "Registry transfer suite failed: $($failed -join ', ')"}
$global:LASTEXITCODE=0
