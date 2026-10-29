param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-05'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-named-objects-probe.ps1" -OutputDirectory $RuntimeDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\named-objects-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$stem='WinedowsTest.'+[Guid]::NewGuid().ToString('N')
$processes=[Collections.Generic.List[object]]::new();$results=@();$prefixes=@{}
function Start-Fixture([string]$program,[string[]]$arguments,[string]$label,[string]$control){
    $start=[Diagnostics.ProcessStartInfo]::new($program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle='Hidden';$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($argument in $arguments){$start.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($start)
    $item=@{Process=$process;Output=$process.StandardOutput.ReadToEndAsync();Error=$process.StandardError.ReadToEndAsync();Label=$label;Control=$control;Log=(Join-Path $suite "$label.log")}
    $processes.Add($item);return $item
}
function Finish-Fixture($item){
    if(!$item.Process.WaitForExit(15000)){throw "Fixture did not finish: $($item.Label)"}
    $text=$item.Output.Result+$item.Error.Result;[IO.File]::WriteAllText($item.Log,$text)
    Write-Host "$($item.Label): exit=$($item.Process.ExitCode)"
    if($item.Process.ExitCode -ne 0 -or $text -match '(?m)^FAIL '){throw "Named object fixture failed: $($item.Log)"}
}
function Ready-Fixture($item){
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while(!(Test-Path -LiteralPath ($item.Control+'.ready'))){
        if($item.Process.HasExited){Finish-Fixture $item;throw "Fixture exited before readiness: $($item.Label)"}
        if($timer.ElapsedMilliseconds -gt 20000){throw "Fixture did not become ready: $($item.Label)"}
        Start-Sleep -Milliseconds 50
    }
}
try{
    foreach($id in @('a','b')){
        $prefix=Join-Path $suite "prefix-$id";$prefixes[$id]=$prefix
        & $runtime --prefix $prefix init *> (Join-Path $suite "init-$id.log");if($LASTEXITCODE){throw 'Prefix init failed'}
        $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
        foreach($bits in @(32,64)){
            Copy-Item -LiteralPath "$RuntimeDirectory\named-objects-probe$bits.exe" -Destination "$tests\named$bits.exe"
            Copy-Item -LiteralPath "$RuntimeDirectory\named-objects-managed$bits.exe" -Destination "$tests\managed$bits.exe"
        }
    }
    $hostFixture=Start-Fixture "$RuntimeDirectory\named-objects-probe64.exe" @('host',$stem,'7711',(Join-Path $suite 'host')) 'host' (Join-Path $suite 'host')
    Ready-Fixture $hostFixture
    foreach($kind in @('native','managed')){
        $pair=@()
        foreach($id in @('a','b')){
            $bits=if($id -eq 'a'){32}else{64};$other=if($bits -eq 32){64}else{32};$value=if($id -eq 'a'){1000}else{2000}
            $prefix=$prefixes[$id];$probe=if($kind -eq 'managed'){'managed'}else{'named'}
            $control=Join-Path $prefix "drive_c\tests\$kind"
            $item=Start-Fixture $runtime @('--prefix',$prefix,'run',"C:\tests\$probe$bits.exe",'server',$stem,"$value","C:\tests\$kind","C:\tests\$probe$other.exe") "$kind-$id-$bits" $control
            $pair+=,$item;Ready-Fixture $item
        }
        foreach($item in $pair){[IO.File]::WriteAllText($item.Control+'.stop','stop')}
        foreach($item in $pair){Finish-Fixture $item;$results+=@{case=$item.Label;passed=$true;log=$item.Log}}
    }
    [IO.File]::WriteAllText($hostFixture.Control+'.stop','stop');Finish-Fixture $hostFixture
    $nativeTimer=[regex]::Match([IO.File]::ReadAllText($hostFixture.Log),'TIMER_EX_STATUS (\d+)').Groups[1].Value
    foreach($item in $processes|Where-Object {$_.Label -like 'native-*'}){
        $timer=[regex]::Match([IO.File]::ReadAllText($item.Log),'TIMER_EX_STATUS (\d+)').Groups[1].Value
        if(!$nativeTimer -or $timer -ne $nativeTimer){throw 'Named high-resolution timer behavior differs from native baseline'}
    }
    $results+=@{case='host-preserved';passed=$true;log=$hostFixture.Log}
    @{passed=$true;runtime=$RuntimeDirectory;cases=$results;samePrefixCrossBitness=$true;twoPrefixesAndHost=$true;actualClr=$true;highResolutionNamedTimerNativeStatus=$nativeTimer;noGlobalInput=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Named object tests passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json');throw
}finally{
    foreach($item in $processes){
        if(!$item.Process.HasExited){[IO.File]::WriteAllText($item.Control+'.stop','stop');if(!$item.Process.WaitForExit(3000)){$item.Process.Kill($true);$item.Process.WaitForExit()}}
        [IO.File]::WriteAllText($item.Log,$item.Output.Result+$item.Error.Result);$item.Process.Dispose()
    }
}
