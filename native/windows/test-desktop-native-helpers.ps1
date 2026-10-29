param([string]$RuntimeDirectory,[string]$GraphicsFixtures=(Join-Path $PSScriptRoot 'test-output\runtime-07'),[string]$GdiplusFixtures=(Join-Path $PSScriptRoot 'test-output\helpers-08\fixtures'),[string]$CasePattern='*',[string]$WineSourceDirectory)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
. "$PSScriptRoot\wine-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$font=Join-Path $wineSource 'dlls\gdi32\tests\wine_test.ttf';$hash=(Get-FileHash -LiteralPath $font).Hash
$output=Join-Path $PSScriptRoot ('test-output\dpi-08\native-helpers-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$cases=[Collections.Generic.List[object]]::new()
& $runtime --prefix $prefix init|Out-Host;if($LASTEXITCODE){throw 'Helper prefix initialization failed'}
$app=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $app|Out-Null
Copy-Item -LiteralPath $font -Destination "$app\fixture.ttf"
function Invoke-Helper([string]$Name,[string]$Overrides,[string]$Program,[string[]]$Arguments){
    if($Name -notlike $CasePattern){return}
    $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$runtime;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.Environment['WINEDLLOVERRIDES']=$Overrides;$start.Environment.Remove('WINEDOWS_TRACE_DESKTOP')|Out-Null
    if($Name -like 'd3d11*'){$start.Environment['D3D11_PROBE_DUMP']="C:\tests\$Name.dmp"}
    foreach($arg in @('--prefix',$prefix,'--desktop','NativeHelpers,800x600','run',$Program)+$Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$timeout=!$process.WaitForExit(30000)
    if($timeout){$process.Kill($true);$process.WaitForExit()};$code=$process.ExitCode;$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
    $text|Set-Content -LiteralPath "$output\$Name.log";$passed=!$timeout -and $code -eq 0
    if($Name -like 'gdiplus*'){$passed=$passed -and $text -match 'PASS GDI\+ helper provider' -and (Test-Path -LiteralPath "$app\$Name.png")}
    $cases.Add(@{name=$Name;passed=$passed;exitCode=$code;timedOut=$timeout});Write-Host "$Name exit=$code passed=$passed"
}
foreach($bits in @(32,64)){
    $native=(& "$GdiplusFixtures\gdiplus-probe$bits.exe" --native-path).Trim();if($LASTEXITCODE -or !$native){throw 'Native GDI+ discovery failed'}
    $arch=if($bits -eq 32){'x86'}else{'x64'}
    foreach($static in @($false,$true)){
        $kind=if($static){'static'}else{'dynamic'};$gdiplus=if($static){"gdiplus-import-probe$bits.exe"}else{"gdiplus-probe$bits.exe"};$d3d=if($static){"d3d11-import-probe$bits.exe"}else{"d3d11-probe$bits.exe"}
        Copy-Item -LiteralPath "$GdiplusFixtures\$gdiplus" -Destination $app;Copy-Item -LiteralPath "$GraphicsFixtures\$d3d" -Destination $app
        foreach($provider in @('wine','native')){
            $flag=if($provider -eq 'wine'){'b'}else{'n'};$helper=if($provider -eq 'wine'){"$RuntimeDirectory\builtin\$arch\gdiplus.dll"}else{$native}
            $name="gdiplus-$provider-$bits-$kind";Invoke-Helper $name "gdiplus=$flag" "C:\tests\$gdiplus" @($helper,'C:\tests\fixture.ttf',"C:\tests\$name.png")
            Invoke-Helper "d3d11-$provider-$bits-$kind" "dxgi,d3d11=$flag" "C:\tests\$d3d" @("--$provider")
        }
    }
}
$unchanged=(Get-FileHash -LiteralPath $font).Hash -eq $hash -and (Get-FileHash -LiteralPath "$app\fixture.ttf").Hash -eq $hash
$passed=$unchanged -and !($cases|Where-Object {!$_.passed});@{passed=$passed;runtime=$RuntimeDirectory;cases=$cases;fontUnchanged=$unchanged;noInputOrCapture=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$output\result.json"
Write-Host "Desktop native helper evidence: $output";if(!$passed){throw 'Desktop native helper regression'}
$global:LASTEXITCODE=0
