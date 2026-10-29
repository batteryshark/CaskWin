param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if(!(Test-Path -LiteralPath $compiler)){throw 'The real CLR smoke fixture requires the Windows .NET Framework 4 C# compiler.'}
$suite=Join-Path $PSScriptRoot ('test-output\managed-startup-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$cases=[Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Path $suite|Out-Null
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Cannot initialize managed startup prefix'}
$programs=Join-Path $prefix 'drive_c\managed'
New-Item -ItemType Directory -Path $programs|Out-Null
$variants=@(
    @{name='anycpu';platform='anycpu';bits=64},
    @{name='prefer32';platform='anycpu32bitpreferred';bits=32},
    @{name='required32';platform='x86';bits=32}
)
foreach($variant in $variants){
    $exe=Join-Path $programs ($variant.name+'.exe')
    & $compiler /nologo /optimize+ /target:exe ('/platform:'+$variant.platform) ('/out:'+$exe) (Join-Path $PSScriptRoot 'managed-startup-probe.cs')
    if($LASTEXITCODE){throw "Cannot compile real CLR $($variant.name) fixture"}
}
$saved=$env:WINEDLLOVERRIDES
try{
    $env:WINEDLLOVERRIDES=$null
    foreach($launcher in @(32,64)){
        foreach($parent in $variants){
            foreach($child in $variants){
                $label="launcher$launcher-$($parent.name)-to-$($child.name)"
                $arguments=@('--prefix',$prefix,'run',"C:\managed\$($parent.name).exe",[string]$parent.bits,"C:\managed\$($child.name).exe",[string]$child.bits)
                $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $RuntimeDirectory "wine$launcher.exe"))
                $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
                foreach($arg in $arguments){$start.ArgumentList.Add($arg)}
                $process=[Diagnostics.Process]::Start($start)
                $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
                if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
                [IO.File]::WriteAllText((Join-Path $suite ($label+'.log')),$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult())
                $process.Dispose()
                $cases.Add(@{fixture=$label;launcher=$launcher;parent=$parent.name;child=$child.name;exitCode=$code;passed=($code -eq 0)})
                Write-Host "$(if($code){'FAIL'}else{'PASS'}) $label ($code)"
                if($code){Get-Content -LiteralPath (Join-Path $suite ($label+'.log'));throw "Real CLR startup failed: $label"}
            }
        }
    }
    @{passed=$true;cases=$cases;runtimeSource=$RuntimeDirectory;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Real CLR startup suite passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;runtimeSource=$RuntimeDirectory;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}finally{$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
