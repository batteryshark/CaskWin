param([string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$Vendor)
$ErrorActionPreference='Stop';$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$cases=[Collections.Generic.List[object]]::new()
if($Vendor -and $Vendor -notmatch '^(0x)?[0-9a-fA-F]{1,8}$'){throw 'Expected a numeric Vulkan vendor ID'}
$suffix=if($Vendor){"-$Vendor"}else{''}
foreach($bits in @(32,64)){
 $arch=if($bits -eq 32){'x86'}else{'x64'};$directory=Join-Path $StageDirectory $arch
 foreach($provider in @('native','winevulkan','vulkan-1')){
  $module=if($provider -eq 'native'){Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\vulkan-1.dll'}else{'System32\vulkan-1.dll'})}else{Join-Path $directory "$provider.dll"}
  $info=[Diagnostics.ProcessStartInfo]::new((Join-Path $directory "probe$bits.exe"));$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true;$info.ArgumentList.Add($module);if($provider -ne 'native'){$info.ArgumentList.Add('--wine')}
  if($Vendor){$info.ArgumentList.Add('--vendor');$info.ArgumentList.Add($Vendor)}
  $p=[Diagnostics.Process]::Start($info);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(45000)){$p.Kill($true);$p.WaitForExit();$code=124}else{$code=$p.ExitCode}
  $out=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$p.Dispose();$label="standalone$bits-$provider$suffix";[IO.File]::WriteAllText((Join-Path $StageDirectory "$label.log"),$out)
  $cases.Add(@{case=$label;exitCode=$code;passed=($code -eq 0)});Write-Host "$label exit=$code";if($code){Write-Host $out}
 }
}
@{passed=(@($cases|Where-Object{!$_.passed}).Count -eq 0);vendor=$Vendor;cyclesPerCase=3;transferPixelsPerCycle=64;shaderPixelsPerCycle=64;cases=$cases}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $StageDirectory "standalone-result$suffix.json")
if(@($cases|Where-Object{!$_.passed}).Count){throw 'Experimental Vulkan standalone cases failed'}
$global:LASTEXITCODE=0
