param(
    [string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\compiler47-fixtures'),
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-compiler47-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\compiler47-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Path $suite|Out-Null
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Cannot initialize compiler47 prefix'}
$apps=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $apps|Out-Null
foreach($bits in @(32,64)){foreach($name in @("compiler47-probe$bits.exe","compiler47-import-probe$bits.exe")){Copy-Item -LiteralPath (Join-Path $FixtureDirectory $name) -Destination $apps}}
$renderer=Join-Path $suite 'renderer.reg'
@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\Direct3D]
"renderer"="gl"
'@|Set-Content -LiteralPath $renderer
& $runtime --prefix $prefix reg import $renderer
if($LASTEXITCODE){throw 'Cannot configure Wine hardware renderer'}
$displayProbe=Join-Path $RuntimeDirectory 'display-probe64.exe'
$before=& $displayProbe|Where-Object{$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Cannot snapshot host display'}
$cases=[Collections.Generic.List[object]]::new()
function Invoke-CompilerCase([string]$label,[string]$loader,[int]$bits,[bool]$static,[string]$orders,[string]$compiler,[string]$graphics,[bool]$reject=$false){
    $program=if($static){"compiler47-import-probe$bits.exe"}else{"compiler47-probe$bits.exe"}
    $start=[Diagnostics.ProcessStartInfo]::new($loader);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in @('--prefix',$prefix,'--desktop','off','run',"C:\tests\$program",$compiler,$graphics)){$start.ArgumentList.Add($arg)}
    $start.Environment['WINEDLLOVERRIDES']=$orders
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText((Join-Path $suite ($label+'.log')),$text)
    $passed=if($reject){$code -ne 0 -and $text -match 'No available implementation.*d3dcompiler_47'}else{$code -eq 0}
    $cases.Add(@{fixture=$label;architecture=$bits;staticImport=$static;orders=$orders;exitCode=$code;passed=$passed})
    Write-Host "$(if($passed){'PASS'}else{'FAIL'}) $label ($code)"
    if(!$passed){Write-Output $text;throw "Compiler47 fixture failed: $label"}
}
try{
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
        foreach($compiler in @('wine','native')){foreach($graphics in @('wine','native')){foreach($static in @($false,$true)){
            $compilerPath=if($compiler -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3dcompiler_47.dll"}else{Join-Path $system 'd3dcompiler_47.dll'}
            $graphicsPath=if($graphics -eq 'wine'){Join-Path $RuntimeDirectory "builtin\$arch\d3d11.dll"}else{Join-Path $system 'd3d11.dll'}
            $compilerOrder=if($compiler -eq 'wine'){'b,n'}else{'n,b'};$graphicsOrder=if($graphics -eq 'wine'){'b'}else{'n'}
            Invoke-CompilerCase "$compiler-compiler-$graphics-d3d11-$bits-static$static" $runtime $bits $static "d3dcompiler_47=$compilerOrder;dxgi,d3d11=$graphicsOrder" $compilerPath $graphicsPath
        }}}
    }
    $fallback=Join-Path $suite 'runtime-without-compiler47';New-Item -ItemType Directory -Path $fallback|Out-Null
    foreach($name in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback}
    foreach($arch in @('x86','x64')){
        $destination=Join-Path $fallback "builtin\$arch";New-Item -ItemType Directory -Path $destination|Out-Null
        Get-ChildItem -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch") -File|Where-Object{$_.Name -ne 'd3dcompiler_47.dll'}|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $destination}
    }
    foreach($bits in @(32,64)){
        $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'});$loader=Join-Path $fallback 'wine64.exe'
        Invoke-CompilerCase "missing-builtin-native-fallback-$bits" $loader $bits $true 'd3dcompiler_47=b,n;dxgi,d3d11=n' (Join-Path $system 'd3dcompiler_47.dll') (Join-Path $system 'd3d11.dll')
        Invoke-CompilerCase "missing-strict-builtin-$bits" $loader $bits $false 'd3dcompiler_47=b;dxgi,d3d11=n' (Join-Path $system 'd3dcompiler_47.dll') (Join-Path $system 'd3d11.dll') $true
    }
    $after=& $displayProbe|Where-Object{$_ -like 'DISPLAY *'}
    if($LASTEXITCODE -or $after -ne $before){throw 'Host display changed during compiler47 fixtures'}
    @{passed=$true;hostDisplayPreserved=$true;cases=$cases;runtimeSource=$RuntimeDirectory;knownLimitations=@('Wine HLSL structured-buffer stores report E_NOTIMPL upstream; the executed compute fixture uses a typed integer UAV.');utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Compiler47 suite passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;runtimeSource=$RuntimeDirectory;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}
$global:LASTEXITCODE=0
