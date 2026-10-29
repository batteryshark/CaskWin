param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
      [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-10\fixtures'),
      [string]$HelperDirectory,
      [string]$OutputDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-10'),
      [switch]$PreCatalog,[switch]$CpuOnly,[switch]$Desktop,[string]$CasePattern='*')
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$HelperDirectory){$HelperDirectory=if($PreCatalog){Join-Path $PSScriptRoot 'test-output\helpers-10\runtime\builtin'}else{Join-Path $RuntimeDirectory 'builtin'}}
$HelperDirectory=[IO.Path]::GetFullPath($HelperDirectory);$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$suite=Join-Path $OutputDirectory ('glu32-'+[Guid]::NewGuid().ToString('N'));$prefix=Join-Path $suite 'prefix'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'; & $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'GLU prefix initialization failed'}
$cases=[Collections.Generic.List[object]]::new()
function Invoke-Case([string]$Label,[string]$Loader,[string]$Program,[string]$Helper,[string]$Provider,[string]$Overrides,[bool]$Failure=$false,[bool]$ForceCpu=$false){
    if($Label -notlike $CasePattern){return}
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=if($Loader){$Loader}else{$Program}
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.Environment['WINEDLLOVERRIDES']=$Overrides;$info.Environment['WINEDEBUG']='err+all,warn+glu'
    if($Loader){$arguments=@('--prefix',$prefix);if($Desktop){$arguments+=@('--desktop','GLU,800x600')};$arguments+=@('run',$Program);foreach($arg in $arguments){$info.ArgumentList.Add($arg)}}
    $info.ArgumentList.Add($Helper);$info.ArgumentList.Add($Provider);if($CpuOnly -or $ForceCpu){$info.ArgumentList.Add('--cpu-only')}
    $process=[Diagnostics.Process]::Start($info);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $timeout=!$process.WaitForExit(30000);if($timeout){$process.Kill($true);$process.WaitForExit()};$code=$process.ExitCode
    $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
    $log=Join-Path $suite "$Label.log";[IO.File]::WriteAllText($log,$text)
    $passed=if($Failure){!$timeout -and $code -eq 1 -and $text -match 'No available implementation for glu32.dll'}else{!$timeout -and $code -eq 0 -and $text -match 'SUMMARY failures=0'}
    $cases.Add(@{name=$Label;passed=$passed;exitCode=$code;timedOut=$timeout;expectedMissingBuiltin=$Failure;log=$log;summary=@($text -split "`r?`n"|Where-Object {$_ -match '^(GLU_MODULE|OPENGL_IMPORTS|OPENGL_RENDERER|MATH|NURBS|TESS |SCALE|MIPMAP|PIXEL|SUMMARY|FAIL)'})})
    Write-Host "$Label exit=$code passed=$passed"
    if(!$passed){Write-Host $text;throw "GLU fixture failed: $Label"}
}
try{
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
        $native=Join-Path $system 'glu32.dll'
        $preferences=if($CpuOnly){@('default')}else{@('default','discrete')}
        foreach($provider in @('native','wine')){
            $app=Join-Path $prefix "drive_c\glu-$provider-$bits";New-Item -ItemType Directory -Force -Path $app|Out-Null
            $helper=if($provider -eq 'wine'){Join-Path $HelperDirectory "$arch\glu32.dll"}else{$native}
            if($PreCatalog -and $provider -eq 'wine'){Copy-Item -LiteralPath $helper -Destination (Join-Path $app 'glu32.dll');$helper=Join-Path $app 'glu32.dll'}
            $overrides='glu32='+$(if($provider -eq 'wine' -and !$PreCatalog){'b'}else{'n'})+';opengl32=n'
            foreach($kind in @('dynamic','import')){foreach($preference in $preferences){
                $name="glu32-$kind-$preference$bits.exe";$source=Join-Path $FixtureDirectory $name
                Copy-Item -LiteralPath $source -Destination $app
                if($provider -eq 'native'){Invoke-Case "host-$kind-$preference-$bits" '' $source $native native ''}
                Invoke-Case "$provider-$kind-$preference-$bits" $runtime (Join-Path $app $name) $helper $provider $overrides
            }}
        }
    }
    if(!$PreCatalog){
        $fallback=Join-Path $suite 'runtime-without-glu';New-Item -ItemType Directory -Force -Path $fallback|Out-Null
        foreach($name in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $fallback}
        foreach($bits in @(32,64)){
            $system=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64'}else{'System32'})
            foreach($kind in @('dynamic','import')){
                $program=Join-Path $prefix "drive_c\glu-native-$bits\glu32-$kind-default$bits.exe"
                Invoke-Case "missing-fallback-$kind-$bits" (Join-Path $fallback 'wine64.exe') $program (Join-Path $system 'glu32.dll') native 'glu32=b,n;opengl32=n' $false $true
                Invoke-Case "missing-strict-$kind-$bits" (Join-Path $fallback 'wine64.exe') $program (Join-Path $system 'glu32.dll') native 'glu32=b;opengl32=n' $true $true
            }
        }
    }
    if(!$cases.Count){throw 'No GLU cases selected'}
    @{passed=$true;cases=$cases;runtime=$RuntimeDirectory;helperDirectory=$HelperDirectory;preCatalog=[bool]$PreCatalog;cpuOnly=[bool]$CpuOnly;desktop=[bool]$Desktop;knownGap='Wine NURBS is unsupported; module fallback does not retry individual API failures';noHostSettingsInputOrCapture=$true}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $suite 'result.json')
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;runtime=$RuntimeDirectory}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw
}
Write-Host "GLU evidence: $suite";$global:LASTEXITCODE=0
