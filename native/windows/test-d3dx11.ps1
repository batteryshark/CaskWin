param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
      [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-07\fixtures'),
      [string]$HelperDirectory,
      [string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-07'),
      [switch]$PreCatalog)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(!$HelperDirectory){$HelperDirectory=Join-Path $RuntimeDirectory 'builtin'}
$HelperDirectory=[IO.Path]::GetFullPath($HelperDirectory)
$suite=Join-Path $OutputDirectory ('d3dx11-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'D3DX11 prefix initialization failed'}
$cases=[Collections.Generic.List[object]]::new()
function Invoke-Case([string]$Label,[string]$Loader,[int]$Bits,[string]$Program,
        [string]$Helper,[string]$Graphics,[string]$Compiler,[string]$Overrides,[bool]$Wine,[bool]$Failure=$false){
 $info=[Diagnostics.ProcessStartInfo]::new($Loader)
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
 $info.Environment['WINEDLLOVERRIDES']=$Overrides;[void]$info.Environment.Remove('WINE_D3D_CONFIG')
 foreach($arg in @('--prefix',$prefix,'run',$Program,$Helper,$Graphics,$Compiler,$(if($Wine){'wine'}else{'native'}))){$info.ArgumentList.Add($arg)}
 $process=[Diagnostics.Process]::Start($info)
 $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$exitCode=124}else{$exitCode=$process.ExitCode}
 $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
 [IO.File]::WriteAllText((Join-Path $suite "$Label.log"),$output)
 $passed=if($Failure){$exitCode -eq 1 -and $output -match 'No available implementation for d3dx11_43.dll'}
         else{$exitCode -eq 0 -and $output -match 'PASS D3DX11 helper version'}
 $cases.Add(@{case=$Label;exitCode=$exitCode;passed=$passed;expectedFailure=$Failure})
 Write-Host "$Label exit=$exitCode passed=$passed"
 if(!$passed){Write-Host $output;throw "D3DX11 case failed: $Label"}
}
try{
 foreach($bits in @(32,64)){
  $arch=if($bits -eq 32){'x86'}else{'x64'}
  $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
  foreach($helperKind in @('wine','native')){
   $app=Join-Path $prefix "drive_c\helper-$helperKind-$bits";New-Item -ItemType Directory -Force -Path $app|Out-Null
   $sourceHelper=if($helperKind -eq 'wine'){Join-Path $HelperDirectory "$arch\d3dx11_43.dll"}else{Join-Path $system 'd3dx11_43.dll'}
   if($PreCatalog){Copy-Item -LiteralPath $sourceHelper -Destination (Join-Path $app 'd3dx11_43.dll');$helper=Join-Path $app 'd3dx11_43.dll'}else{$helper=$sourceHelper}
   foreach($static in @($false,$true)){
    $name=if($static){"d3dx11-import-probe$bits.exe"}else{"d3dx11-probe$bits.exe"}
    Copy-Item -LiteralPath (Join-Path $FixtureDirectory $name) -Destination $app
    foreach($graphicsKind in @('wine','native')){
     $graphics=if($graphicsKind -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3d11.dll"}else{Join-Path $system 'd3d11.dll'}
     $compiler=if($helperKind -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3dcompiler_47.dll"}else{Join-Path $system 'd3dcompiler_43.dll'}
     $overrides='d3dcompiler_43=n;d3dcompiler_47='+$(if($helperKind -eq 'wine'){'b'}else{'n'})+';dxgi,d3d11='+$(if($graphicsKind -eq 'wine'){'b'}else{'n'})
     if(!$PreCatalog){$overrides+=';d3dx11_43='+$(if($helperKind -eq 'wine'){'b'}else{'n'})}
     $label="$helperKind-helper-$graphicsKind-d3d11-$bits-"+$(if($static){'static'}else{'dynamic'})
     Invoke-Case $label $runtime $bits (Join-Path $app $name) $helper $graphics $compiler $overrides ($helperKind -eq 'wine')
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
    $name=if($static){"d3dx11-import-probe$bits.exe"}else{"d3dx11-probe$bits.exe"}
    $kind=if($static){'static'}else{'dynamic'}
    $program=Join-Path $prefix "drive_c\helper-native-$bits\$name"
    Invoke-Case "missing-helper-fallback-$bits-$kind" (Join-Path $fallback 'wine64.exe') $bits $program (Join-Path $system 'd3dx11_43.dll') (Join-Path $system 'd3d11.dll') (Join-Path $system 'd3dcompiler_43.dll') 'd3dx11_43=b,n;dxgi,d3d11=n;d3dcompiler_43=n' $false
    Invoke-Case "missing-strict-helper-$bits-$kind" (Join-Path $fallback 'wine64.exe') $bits $program (Join-Path $system 'd3dx11_43.dll') (Join-Path $system 'd3d11.dll') (Join-Path $system 'd3dcompiler_43.dll') 'd3dx11_43=b;dxgi,d3d11=n;d3dcompiler_43=n' $false $true
   }
  }
 }
 @{passed=$true;cases=$cases;preCatalog=[bool]$PreCatalog;runtime=$RuntimeDirectory;knownUpstreamGaps=@('D3DX11CreateThreadPump stub','D3DX11SaveTextureToFileA/W E_NOTIMPL')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
}catch{
 @{passed=$false;error=$_.Exception.Message;cases=$cases}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 throw
}
$global:LASTEXITCODE=0
