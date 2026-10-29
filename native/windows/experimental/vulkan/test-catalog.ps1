param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\..\bin'),
      [string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),
      [string]$AdditionalVendor)
$ErrorActionPreference='Stop'
if($AdditionalVendor -and $AdditionalVendor -notmatch '^(0x)?[0-9a-fA-F]{1,8}$'){throw 'Expected a numeric Vulkan vendor ID'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
$suite=Join-Path $StageDirectory ('catalog-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$cases=[Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Force -Path $suite|Out-Null
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Vulkan catalog prefix init failed'}
$apps=Join-Path $prefix 'drive_c\vulkan'
New-Item -ItemType Directory -Force -Path $apps|Out-Null
foreach($bits in @(32,64)){
 foreach($name in @("probe$bits.exe","import-probe$bits.exe")){
  Copy-Item -LiteralPath (Join-Path $StageDirectory $name) -Destination $apps
 }
}
function Invoke-Case([string]$Label,[string]$Loader,[int]$Bits,[bool]$Static,[string]$Order,[bool]$Wine,[bool]$Failure=$false,[string]$Vendor){
 $name=if($Static){"import-probe$Bits.exe"}else{"probe$Bits.exe"}
 $info=[Diagnostics.ProcessStartInfo]::new($Loader)
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
 $info.Environment['WINEDLLOVERRIDES']="vulkan-1=$Order"
 [void]$info.Environment.Remove('WINE_D3D_CONFIG')
 foreach($arg in @('--prefix',$prefix,'run',"C:\vulkan\$name",'vulkan-1.dll')){$info.ArgumentList.Add($arg)}
 if($Wine){$info.ArgumentList.Add('--wine')}
 if($Vendor){$info.ArgumentList.Add('--vendor');$info.ArgumentList.Add($Vendor)}
 $process=[Diagnostics.Process]::Start($info)
 $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$exitCode=124}else{$exitCode=$process.ExitCode}
 $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
 [IO.File]::WriteAllText((Join-Path $suite "$Label.log"),$output)
 $passed=if($Failure){$exitCode -ne 0 -and $exitCode -ne 124 -and $output -match 'No available implementation for (vulkan-1|winevulkan)\.dll' -and $output -notmatch 'PASS all3'}else{$exitCode -eq 0 -and $output -match 'PASS all3'}
 $cases.Add(@{case=$Label;exitCode=$exitCode;passed=$passed;expectedFailure=$Failure})
 Write-Host "$Label exit=$exitCode passed=$passed"
 if(!$passed){Write-Host $output;throw "Vulkan catalog case failed: $Label"}
}
try{
 foreach($bits in @(32,64)){
  foreach($static in @($false,$true)){
   $kind=if($static){'static'}else{'dynamic'}
   foreach($order in @('b','b,n','n','n,b')){
    Invoke-Case "$kind-$bits-$($order.Replace(',',''))" $runtime $bits $static $order ($order.StartsWith('b'))
   }
  }
 }
 if($AdditionalVendor){
  foreach($bits in @(32,64)){
   foreach($static in @($false,$true)){
    $kind=if($static){'static'}else{'dynamic'}
    Invoke-Case "builtin-$kind-$bits-vendor$AdditionalVendor" $runtime $bits $static 'b' $true $false $AdditionalVendor
   }
  }
 }
 $fallback=Join-Path $suite 'runtime-without-vulkan'
 New-Item -ItemType Directory -Force -Path $fallback|Out-Null
 foreach($name in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){
  Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback
 }
 foreach($bits in @(32,64)){
  foreach($static in @($false,$true)){
   $kind=if($static){'static'}else{'dynamic'}
   Invoke-Case "missing-builtin-fallback-$kind-$bits" (Join-Path $fallback 'wine64.exe') $bits $static 'b,n' $false
   Invoke-Case "missing-strict-builtin-$kind-$bits" (Join-Path $fallback 'wine64.exe') $bits $static 'b' $true $true
  }
 }
 # A present forwarder with its real Wine implementation missing must not
 # defeat permissive native fallback or make strict builtin appear successful.
 foreach($bits in @(32,64)){
  $arch=if($bits -eq 32){'x86'}else{'x64'}
  $directory=Join-Path $fallback "builtin\$arch"
  New-Item -ItemType Directory -Force -Path $directory|Out-Null
  Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch\vulkan-1.dll") -Destination $directory
 }
 foreach($bits in @(32,64)){
  foreach($static in @($false,$true)){
   $kind=if($static){'static'}else{'dynamic'}
   Invoke-Case "missing-dependency-fallback-$kind-$bits" (Join-Path $fallback 'wine64.exe') $bits $static 'b,n' $false
   Invoke-Case "missing-dependency-strict-$kind-$bits" (Join-Path $fallback 'wine64.exe') $bits $static 'b' $true $true
  }
 }
 @{passed=$true;cases=$cases;runtime=$RuntimeDirectory}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
}catch{
 @{passed=$false;error=$_.Exception.Message;cases=$cases}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 throw
}
$global:LASTEXITCODE=0
