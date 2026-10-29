param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
      [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-08\fixtures'),
      [string]$HelperDirectory,
      [string]$FontFile,
      [string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-08'),
      [string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
if(!$FontFile){
 . "$PSScriptRoot\wine-source.ps1"
 $wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
 $FontFile=Join-Path $wineSource 'dlls\gdi32\tests\wine_test.ttf'
}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(!$HelperDirectory){$HelperDirectory=Join-Path $RuntimeDirectory 'builtin'}
$HelperDirectory=[IO.Path]::GetFullPath($HelperDirectory)
$fontSourceHash=(Get-FileHash -LiteralPath $FontFile -Algorithm SHA256).Hash
$id=[Guid]::NewGuid().ToString('N');$suite=Join-Path $OutputDirectory ('gdiplus-'+$id)
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$virtualRoot="C:\gdiplus-$id";$physicalRoot=Join-Path $prefix "drive_c\gdiplus-$id"
New-Item -ItemType Directory -Force -Path $suite|Out-Null
if(Test-Path -LiteralPath $virtualRoot){throw 'Unique host control path unexpectedly exists'}
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'GDI+ prefix initialization failed'}
New-Item -ItemType Directory -Force -Path $physicalRoot|Out-Null
Copy-Item -LiteralPath $FontFile -Destination (Join-Path $physicalRoot 'fixture.ttf')
Copy-Item -LiteralPath $FontFile -Destination (Join-Path $suite 'fixture.ttf')
$cases=[Collections.Generic.List[object]]::new();$native=@{}
function Invoke-Case([string]$Label,[string]$Loader,[string]$Program,[string]$Helper,[string]$Overrides,[bool]$Failure=$false){
 $info=[Diagnostics.ProcessStartInfo]::new($(if($Loader){$Loader}else{$Program}))
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
 $info.Environment['WINEDLLOVERRIDES']=$Overrides
 if($Loader){foreach($arg in @('--prefix',$prefix,'run',$Program)){$info.ArgumentList.Add($arg)}}
 $font=if($Loader){Join-Path $virtualRoot 'fixture.ttf'}else{Join-Path $suite 'fixture.ttf'}
 $png=if($Loader){Join-Path $virtualRoot "$Label.png"}else{Join-Path $suite "$Label.png"}
 foreach($arg in @($Helper,$font,$png)){$info.ArgumentList.Add($arg)}
 $process=[Diagnostics.Process]::Start($info)
 $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$exitCode=124}else{$exitCode=$process.ExitCode}
 $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
 [IO.File]::WriteAllText((Join-Path $suite "$Label.log"),$output)
 $passed=if($Failure){$exitCode -eq 1 -and $output -match 'No available implementation for gdiplus.dll'}
         else{$exitCode -eq 0 -and $output -match 'PASS GDI\+ helper provider'}
 if($Loader -and !$Failure){$passed=$passed -and (Test-Path -LiteralPath (Join-Path $physicalRoot "$Label.png")) -and !(Test-Path -LiteralPath $virtualRoot)}
 $cases.Add(@{case=$Label;exitCode=$exitCode;passed=$passed;expectedFailure=$Failure})
 Write-Host "$Label exit=$exitCode passed=$passed"
 if(!$passed){Write-Host $output;throw "GDI+ case failed: $Label"}
}
try{
 foreach($bits in @(32,64)){
  # GDI+ is a side-by-side Windows component. Discover the system loader's actual
  # native file in a fresh architecture-matched process, then keep strict identity
  # assertions for every baseline/provider run. Never treat a native redirect as Wine.
  $native[$bits]=(& (Join-Path $FixtureDirectory "gdiplus-probe$bits.exe") --native-path).Trim()
  if($LASTEXITCODE -or !$native[$bits] -or !(Test-Path -LiteralPath $native[$bits])){throw 'Native GDI+ discovery failed'}
  foreach($static in @($false,$true)){
   $name=if($static){"gdiplus-import-probe$bits.exe"}else{"gdiplus-probe$bits.exe"}
   $kind=if($static){'static'}else{'dynamic'}
   Invoke-Case "native-baseline-$bits-$kind" '' (Join-Path $FixtureDirectory $name) $native[$bits] ''
  }
  $arch=if($bits -eq 32){'x86'}else{'x64'}
  foreach($provider in @('wine','native')){
   $app=Join-Path $physicalRoot "$provider-$bits";New-Item -ItemType Directory -Force -Path $app|Out-Null
   $helper=if($provider -eq 'wine'){Join-Path $HelperDirectory "$arch\gdiplus.dll"}else{$native[$bits]}
   foreach($static in @($false,$true)){
    $name=if($static){"gdiplus-import-probe$bits.exe"}else{"gdiplus-probe$bits.exe"}
    $kind=if($static){'static'}else{'dynamic'}
    Copy-Item -LiteralPath (Join-Path $FixtureDirectory $name) -Destination $app
    $overrides=if($provider -eq 'wine'){'gdiplus=b'}else{'gdiplus=n'}
    Invoke-Case "$provider-prefix-$bits-$kind" $runtime (Join-Path $app $name) $helper $overrides
   }
  }
 }
  $fallback=Join-Path $suite 'runtime-without-helper';New-Item -ItemType Directory -Force -Path $fallback|Out-Null
  foreach($name in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback}
  foreach($bits in @(32,64)){foreach($static in @($false,$true)){
   $name=if($static){"gdiplus-import-probe$bits.exe"}else{"gdiplus-probe$bits.exe"}
   $kind=if($static){'static'}else{'dynamic'};$program=Join-Path $physicalRoot "native-$bits\$name"
   Invoke-Case "missing-helper-fallback-$bits-$kind" (Join-Path $fallback 'wine64.exe') $program $native[$bits] 'gdiplus=b,n'
   Invoke-Case "missing-strict-helper-$bits-$kind" (Join-Path $fallback 'wine64.exe') $program $native[$bits] 'gdiplus=b' $true
  }}
 if(Test-Path -LiteralPath $virtualRoot){throw 'Prefix output escaped to host control path'}
 $prefixFontHash=(Get-FileHash -LiteralPath (Join-Path $physicalRoot 'fixture.ttf') -Algorithm SHA256).Hash
 if($fontSourceHash -ne $prefixFontHash -or $fontSourceHash -ne (Get-FileHash -LiteralPath $FontFile -Algorithm SHA256).Hash){throw 'Private or source font changed during tests'}
 $nativeProviders=@($native.GetEnumerator()|Sort-Object Key|ForEach-Object {@{architectureBits=$_.Key;path=$_.Value}})
 @{passed=$true;cases=$cases;runtime=$RuntimeDirectory;nativeProviders=$nativeProviders;prefixFontSHA256=$prefixFontHash;sourceFontUnchanged=$true;hostControlAbsent=$true}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
}catch{
 @{passed=$false;error=$_.Exception.Message;cases=$cases}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 throw
}
$global:LASTEXITCODE=0
