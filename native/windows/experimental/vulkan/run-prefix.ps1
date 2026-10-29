param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\..\bin'),[string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$prefix=Join-Path $StageDirectory 'prefix';$cases=[Collections.Generic.List[object]]::new()
foreach($bits in @(32,64)){
 foreach($module in @('winevulkan','vulkan-1')){
  $label="prefix$bits-$module";$info=[Diagnostics.ProcessStartInfo]::new((Join-Path $RuntimeDirectory 'wine64.exe'))
  $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
  [void]$info.Environment.Remove('WINEDLLOVERRIDES');[void]$info.Environment.Remove('WINE_D3D_CONFIG')
  foreach($arg in @('--prefix',$prefix,'run',"C:\vulkan$bits\probe$bits.exe","C:\vulkan$bits\$module.dll",'--wine')){$info.ArgumentList.Add($arg)}
  $p=[Diagnostics.Process]::Start($info);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(45000)){$p.Kill($true);$p.WaitForExit();$code=124}else{$code=$p.ExitCode}
  $out=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$p.Dispose();[IO.File]::WriteAllText((Join-Path $StageDirectory "$label.log"),$out)
  $cases.Add(@{case=$label;exitCode=$code;passed=($code -eq 0)});Write-Host "$label exit=$code";if($code){Write-Host $out}
 }
}
@{passed=(@($cases|Where-Object{!$_.passed}).Count -eq 0);cases=$cases}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $StageDirectory 'prefix-result.json')
if(@($cases|Where-Object{!$_.passed}).Count){throw 'Experimental Vulkan prefix cases failed'}
$global:LASTEXITCODE=0
