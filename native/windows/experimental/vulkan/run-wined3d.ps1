param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\..\bin'),[string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$prefix=Join-Path $StageDirectory 'prefix';$cases=[Collections.Generic.List[object]]::new()
foreach($bits in @(32,64)){
 $label="prefix$bits-wined3d-vulkan";$info=[Diagnostics.ProcessStartInfo]::new((Join-Path $RuntimeDirectory 'wine64.exe'))
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
 $info.Environment['WINEDLLOVERRIDES']='dxgi,d3d11=b';$info.Environment['WINE_D3D_CONFIG']='renderer=vulkan'
 foreach($arg in @('--prefix',$prefix,'run',"C:\vulkan$bits\d3d11-probe$bits.exe",'--wine')){$info.ArgumentList.Add($arg)}
 $p=[Diagnostics.Process]::Start($info);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(45000)){$p.Kill($true);$p.WaitForExit();$code=124}else{$code=$p.ExitCode}
 $out=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$p.Dispose();[IO.File]::WriteAllText((Join-Path $StageDirectory "$label.log"),$out)
 $cases.Add(@{case=$label;exitCode=$code;passed=($code -eq 0)})
 Write-Host "$label exit=$code";($out -split "`r?`n")|Select-Object -Last 45
}
@{passed=(@($cases|Where-Object{!$_.passed}).Count -eq 0);cases=$cases}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $StageDirectory 'wined3d-result.json')
if(@($cases|Where-Object{!$_.passed}).Count){throw 'WineD3D Vulkan cases failed; inspect retained per-architecture logs'}
$global:LASTEXITCODE=0
