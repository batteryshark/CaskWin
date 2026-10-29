param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
      [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-09\fixtures'),
      [string]$HelperDirectory,
      [string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-09'),
      [switch]$PreCatalog,
      [switch]$CpuOnly)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(!$HelperDirectory){$HelperDirectory=Join-Path $RuntimeDirectory 'builtin'}
$HelperDirectory=[IO.Path]::GetFullPath($HelperDirectory)
$suite=Join-Path $OutputDirectory ('d3dx10-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'D3DX10 prefix initialization failed'}
$cases=[Collections.Generic.List[object]]::new()
$effectFile=Join-Path $prefix 'drive_c\d3dx10-draw.fxo'
Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'd3dx10-draw.fxo') -Destination $effectFile
function Invoke-Case([string]$Label,[string]$Loader,[int]$Bits,[string]$Program,
        [string]$Helper,[string]$Graphics,[string]$Compiler,[string]$Math,[string]$Overrides,[bool]$Wine,[bool]$Failure=$false){
 $info=[Diagnostics.ProcessStartInfo]::new($Loader)
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
 $info.Environment['WINEDLLOVERRIDES']=$Overrides;[void]$info.Environment.Remove('WINE_D3D_CONFIG')
 foreach($arg in @('--prefix',$prefix,'run',$Program,$Helper,$Graphics,$Compiler,$Math,$(if($Wine){'wine'}else{'native'}),'C:\d3dx10-draw.fxo')){$info.ArgumentList.Add($arg)}
 if($CpuOnly){$info.ArgumentList.Add('--cpu-only')}
 $process=[Diagnostics.Process]::Start($info)
 $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$exitCode=124}else{$exitCode=$process.ExitCode}
 $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
 [IO.File]::WriteAllText((Join-Path $suite "$Label.log"),$output)
 $passed=if($Failure){$exitCode -eq 1 -and $output -match 'No available implementation for d3dx10_43.dll'}
         else{$exitCode -eq 0 -and $output -match $(if($CpuOnly){'PASS D3DX10 CPU version'}else{'PASS D3DX10 helper version'})}
 $cases.Add(@{case=$Label;exitCode=$exitCode;passed=$passed;expectedFailure=$Failure})
 Write-Host "$Label exit=$exitCode passed=$passed"
 if(!$passed){Write-Host $output;throw "D3DX10 case failed: $Label"}
}
try{
 foreach($bits in @(32,64)){
  $arch=if($bits -eq 32){'x86'}else{'x64'}
  $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
  foreach($helperKind in @('wine','native')){
   $app=Join-Path $prefix "drive_c\helper-$helperKind-$bits";New-Item -ItemType Directory -Force -Path $app|Out-Null
   $sourceHelper=if($helperKind -eq 'wine'){Join-Path $HelperDirectory "$arch\d3dx10_43.dll"}else{Join-Path $system 'd3dx10_43.dll'}
   if($PreCatalog){Copy-Item -LiteralPath $sourceHelper -Destination (Join-Path $app 'd3dx10_43.dll');$helper=Join-Path $app 'd3dx10_43.dll'}else{$helper=$sourceHelper}
   $math=if($helperKind -eq 'wine'){Join-Path $HelperDirectory "$arch\d3dx9_36.dll"}else{$helper}
   if($PreCatalog -and $helperKind -eq 'wine'){Copy-Item -LiteralPath $math -Destination (Join-Path $app 'd3dx9_36.dll');$math=Join-Path $app 'd3dx9_36.dll'}
   foreach($static in @($false,$true)){
    $name=if($static){"d3dx10-import-probe$bits.exe"}else{"d3dx10-probe$bits.exe"}
    Copy-Item -LiteralPath (Join-Path $FixtureDirectory $name) -Destination $app
    foreach($graphicsKind in @('wine','native')){
     $graphics=if($graphicsKind -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3d10_1.dll"}else{Join-Path $system 'd3d10_1.dll'}
     $compiler=if($helperKind -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3dcompiler_47.dll"}else{Join-Path $system 'd3dcompiler_43.dll'}
     $overrides='d3dcompiler_43=n;d3dcompiler_47='+$(if($helperKind -eq 'wine'){'b'}else{'n'})+';d3d10,d3d10_1,d3d10core,dxgi,d3d11='+$(if($graphicsKind -eq 'wine'){'b'}else{'n'})
     if($PreCatalog -and $helperKind -eq 'wine'){$overrides+=';d3dxof=b'}
     if(!$PreCatalog){$overrides+=';d3dx9_36,d3dx10_43='+$(if($helperKind -eq 'wine'){'b'}else{'n'})}
     $label="$helperKind-helper-$graphicsKind-d3d10-$bits-"+$(if($static){'static'}else{'dynamic'})
     Invoke-Case $label $runtime $bits (Join-Path $app $name) $helper $graphics $compiler $math $overrides ($helperKind -eq 'wine')
    }
   }
  }
 }
 if(!$PreCatalog){
  $fallback=Join-Path $suite 'runtime-without-helper';New-Item -ItemType Directory -Force -Path $fallback|Out-Null
  foreach($name in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback}
  foreach($bits in @(32,64)){
   $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
   foreach($static in @($false,$true)){
    $name=if($static){"d3dx10-import-probe$bits.exe"}else{"d3dx10-probe$bits.exe"}
    $kind=if($static){'static'}else{'dynamic'}
    $program=Join-Path $prefix "drive_c\helper-native-$bits\$name"
    Invoke-Case "missing-helper-fallback-$bits-$kind" (Join-Path $fallback 'wine64.exe') $bits $program (Join-Path $system 'd3dx10_43.dll') (Join-Path $system 'd3d10_1.dll') (Join-Path $system 'd3dcompiler_43.dll') (Join-Path $system 'd3dx10_43.dll') 'd3dx10_43=b,n;d3d10,d3d10_1,d3d10core,dxgi,d3d11=n;d3dcompiler_43=n' $false
    Invoke-Case "missing-strict-helper-$bits-$kind" (Join-Path $fallback 'wine64.exe') $bits $program (Join-Path $system 'd3dx10_43.dll') (Join-Path $system 'd3d10_1.dll') (Join-Path $system 'd3dcompiler_43.dll') (Join-Path $system 'd3dx10_43.dll') 'd3dx10_43=b;d3d10,d3d10_1,d3d10core,dxgi,d3d11=n;d3dcompiler_43=n' $false $true
   }
  }
 }
 @{passed=$true;cases=$cases;preCatalog=[bool]$PreCatalog;cpuOnly=[bool]$CpuOnly;runtime=$RuntimeDirectory;effectFixtureSHA256=(Get-FileHash -LiteralPath $effectFile -Algorithm SHA256).Hash;knownUpstreamGaps=@('FX source embedded CompileShader state assignments E_NOTIMPL; precompiled shader effects are tested','Wine D3D10 ID3D10Effect::IsValid is a stub; GetDesc/pass/state/render behavior is tested','ID3DX10Sprite drawing methods E_NOTIMPL','ID3DX10Font::DrawText E_NOTIMPL','D3DX10 shader-resource-view convenience loaders stub','D3DX10SaveTextureToFileA/W stub')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
}catch{
 @{passed=$false;error=$_.Exception.Message;cases=$cases}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 throw
}
$global:LASTEXITCODE=0
