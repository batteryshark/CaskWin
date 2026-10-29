param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-release'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\context-fixtures'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!('WinedowsContextErrorMode' -as [type])){Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class WinedowsContextErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint value); }'}
$oldErrorMode=[WinedowsContextErrorMode]::SetErrorMode(3)
$suite=Join-Path $PSScriptRoot ('test-output\startup-builtins-08\dll-context-'+[Guid]::NewGuid().ToString('N'));$prefix=Join-Path $suite 'prefix';New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new()
function Layout([string]$Path,[int]$Bits){
    $assembly=Join-Path $Path 'Winedows.Builtin.Boundary';New-Item -ItemType Directory -Force -Path $assembly|Out-Null
    Copy-Item -LiteralPath "$FixtureDirectory\context-probe$Bits.exe" -Destination "$Path\app.exe"
    Copy-Item -LiteralPath "$FixtureDirectory\context-parent$Bits.dll" -Destination "$Path\A.dll"
    Copy-Item -LiteralPath "$FixtureDirectory\context-shadow$Bits.dll" -Destination "$Path\B.dll"
    Copy-Item -LiteralPath "$FixtureDirectory\context-real$Bits.dll" -Destination "$assembly\B.dll"
    $processor=if($Bits -eq 32){'x86'}else{'amd64'}
    [IO.File]::WriteAllText("$assembly\Winedows.Builtin.Boundary.manifest","<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`"><assemblyIdentity type=`"win32`" name=`"Winedows.Builtin.Boundary`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/><file name=`"B.dll`"/></assembly>")
}
function Run([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Order=''){
    $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true;$start.Environment['WINEDLLOVERRIDES']=$Order
    foreach($arg in $Arguments){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $text=$out.GetAwaiter().GetResult()+$err.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText("$suite\$Name.log",$text);$pass=$code -eq 0 -and $text -match 'PASS DLL-owned manifest';$cases.Add(@{name=$Name;passed=$pass;exitCode=$code});Write-Host "$Name exit=$code passed=$pass";if(!$pass){Write-Host $text;throw "DLL context fixture failed: $Name"}
}
try{
    foreach($bits in @(32,64)){$app="$suite\native$bits";Layout $app $bits;Run "native-$bits" "$app\app.exe" @("$app\Winedows.Builtin.Boundary\B.dll")}
    & "$RuntimeDirectory\wine64.exe" --prefix $prefix init;if($LASTEXITCODE){throw 'Prefix init failed'}
    foreach($bits in @(32,64)){Layout "$prefix\drive_c\context$bits" $bits}
    foreach($parent in @(32,64)){foreach($bits in @(32,64)){
        Run "selected-$parent-to-$bits" "$RuntimeDirectory\wine$parent.exe" @('--prefix',$prefix,'run',"C:\context$bits\app.exe","$prefix\drive_c\context$bits\Winedows.Builtin.Boundary\B.dll") 'gdiplus=b'
    }}
    @{passed=$true;runtime=$RuntimeDirectory;cases=$cases;realResource2Manifest=$true;shadowImportsSelectedGdiplus=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";Write-Host "DLL context results: $suite"
}catch{@{passed=$false;runtime=$RuntimeDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";throw}
finally{[void][WinedowsContextErrorMode]::SetErrorMode($oldErrorMode)}
$global:LASTEXITCODE=0
