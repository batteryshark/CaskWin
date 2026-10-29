param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-release'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\fixtures'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WinedowsSystemViewDirectory {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool SetDllDirectory(string value);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern uint GetDllDirectory(uint size,StringBuilder value);
 [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint value);
}
'@
$suite=Join-Path $PSScriptRoot ('test-output\startup-builtins-08\system-view-'+[Guid]::NewGuid().ToString('N'));$prefix=Join-Path $suite 'prefix'
New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new();$savedDirectory=[Text.StringBuilder]::new(32768);[void][WinedowsSystemViewDirectory]::GetDllDirectory(32768,$savedDirectory)
$savedMode=[WinedowsSystemViewDirectory]::SetErrorMode(3);$systemDirectory=Join-Path $env:WINDIR 'System32'
$files=@("$FixtureDirectory\builtin-bound-probe64.exe","$RuntimeDirectory\builtin\x64\gdiplus.dll","$systemDirectory\version.dll",(Join-Path $env:WINDIR 'SysWOW64\version.dll'));$hashes=@{}
foreach($file in $files){$hashes[$file]=(Get-FileHash -LiteralPath $file).Hash}
function Invoke-SystemView([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Overrides){
    $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true;$start.Environment['WINEDLLOVERRIDES']=$Overrides
    foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($start);$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $text=$out.GetAwaiter().GetResult()+$err.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText("$suite\$Name.log",$text)
    $pass=$code -eq 0 -and $text -match 'PASS actual bound-import IAT and GDI\+ startup' -and $text -match ('(?im)^VERSION_MODULE '+[regex]::Escape("$systemDirectory\version.dll")+'\s*$')
    $cases.Add(@{name=$Name;passed=$pass;exitCode=$code});Write-Host "$Name exit=$code passed=$pass";if(!$pass){Write-Host $text;throw "System32 inspection fixture failed: $Name"}
}
try{
    $native=(& "$PSScriptRoot\test-output\helpers-08\fixtures\gdiplus-probe64.exe" --native-path).Trim();if($LASTEXITCODE){throw 'Native GDI+ discovery failed'}
    & "$RuntimeDirectory\wine64.exe" --prefix $prefix init;if($LASTEXITCODE){throw 'Prefix init failed'}
    $app=Join-Path $prefix 'drive_c\system-view';New-Item -ItemType Directory -Path $app|Out-Null
    Copy-Item -LiteralPath "$FixtureDirectory\builtin-bound-probe64.exe" -Destination "$app\probe64.exe"
    if(![WinedowsSystemViewDirectory]::SetDllDirectory($systemDirectory)){throw 'Cannot set the fixture process DLL directory'}
    Invoke-SystemView 'native-64-system32' "$FixtureDirectory\builtin-bound-probe64.exe" @($native) ''
    Invoke-SystemView 'selected-32-to-64-system32' "$RuntimeDirectory\wine32.exe" @('--prefix',$prefix,'run','C:\system-view\probe64.exe',"$RuntimeDirectory\builtin\x64\gdiplus.dll") 'gdiplus=b'
    foreach($file in $files){if((Get-FileHash -LiteralPath $file).Hash -ne $hashes[$file]){throw "Fixture changed source or system file: $file"}}
    @{passed=$true;runtime=$RuntimeDirectory;inheritedDirectory=$systemDirectory;cases=$cases;sourceAndSystemFilesUnchanged=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json"
    Write-Host "System32 inspection results: $suite"
}catch{@{passed=$false;runtime=$RuntimeDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";throw}
finally{[void][WinedowsSystemViewDirectory]::SetDllDirectory($(if($savedDirectory.Length){$savedDirectory.ToString()}else{$null}));[void][WinedowsSystemViewDirectory]::SetErrorMode($savedMode)}
$global:LASTEXITCODE=0
