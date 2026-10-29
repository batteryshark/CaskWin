param(
    [string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\native-startup-fixtures'),
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-native-startup-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\native-startup-'+[Guid]::NewGuid().ToString('N'))
$runtimeCopy=Join-Path $suite 'runtime'
$prefix=Join-Path $suite 'prefix'
New-Item -ItemType Directory -Path $runtimeCopy|Out-Null
foreach($file in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $file) -Destination $runtimeCopy}
$runtime=Join-Path $runtimeCopy 'wine64.exe'
if(-not ('WinedowsStartupErrorMode' -as [type])){Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class WinedowsStartupErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode); }'}
if(-not ('WinedowsStartupDirectory' -as [type])){Add-Type -TypeDefinition 'using System.Text; using System.Runtime.InteropServices; public static class WinedowsStartupDirectory { [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetDllDirectory(uint size, StringBuilder value); [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool SetDllDirectory(string value); }'}
$oldErrorMode=[WinedowsStartupErrorMode]::SetErrorMode(3)
$cases=[Collections.Generic.List[object]]::new()
$tracked=[Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $FixtureDirectory -File|Where-Object {$_.Name -like 'native-startup-*.exe' -or $_.Name -like 'native-startup-*.dll'}|ForEach-Object {$tracked.Add($_.FullName)}
$hostCompilers=@{}
foreach($bits in @(32,64)){
    $hostFile=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\d3dcompiler_43.dll'}else{'System32\d3dcompiler_43.dll'})
    $hostCompilers[$bits]=Test-Path -LiteralPath $hostFile
    if($hostCompilers[$bits]){$tracked.Add($hostFile)}
}
$before=@{};foreach($file in $tracked){$before[$file]=(Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash}
function Invoke-Startup([string]$label,[int]$bits,[string]$program,[string]$value,[string]$expected,[string]$kind,[string]$orders,[string[]]$extra=@(),[uint32]$expectedExit=0,[int]$loaderBits=64,[string]$loaderDirectory){
    $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $runtimeCopy "wine$loaderBits.exe"))
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($arg in @('--prefix',$prefix,'run',$program,$value,$expected,$kind)+$extra){$start.ArgumentList.Add($arg)}
    if($orders){$start.Environment['WINEDLLOVERRIDES']=$orders}else{$start.Environment.Remove('WINEDLLOVERRIDES')|Out-Null}
    $start.Environment['WD_STARTUP_SENTINEL']='prefix-env-preserved';$start.Environment['WD_STARTUP_LONG']='x'*4097
    if($loaderDirectory){
        $oldDirectory=[Text.StringBuilder]::new(32767)
        if([WinedowsStartupDirectory]::GetDllDirectory(32767,$oldDirectory) -ge 32767){throw 'Inherited DLL directory is too long'}
        if(![WinedowsStartupDirectory]::SetDllDirectory($loaderDirectory)){throw 'Cannot set loader fixture directory'}
        try{$process=[Diagnostics.Process]::Start($start)}
        finally{if(![WinedowsStartupDirectory]::SetDllDirectory($(if($oldDirectory.Length){$oldDirectory.ToString()}else{$null}))){throw 'Cannot restore fixture directory'}}
    }else{$process=[Diagnostics.Process]::Start($start)}
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $finished=$process.WaitForExit(45000)
    if(!$finished){$process.Kill($true);$process.WaitForExit()}
    $code=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$process.ExitCode),0)
    $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
    [IO.File]::WriteAllText((Join-Path $suite "$label.log"),$text)
    $passed=$finished -and $code -eq $expectedExit
    $cases.Add(@{fixture=$label;architecture=$bits;orders=$orders;exitCode=('0x{0:x8}' -f $code);expectedExit=('0x{0:x8}' -f $expectedExit);passed=$passed})
    Write-Host "$(if($passed){'PASS'}else{'FAIL'}) $label (0x$('{0:x8}' -f $code))"
    if(!$passed){Write-Output $text;throw "Native startup fixture failed: $label"}
}
try{
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'Cannot initialize startup prefix'}
    $private=@{};$apps=@{};$builtins=@{};$custom=@{};$empty=@{};$isolatedChildren=@{}
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $apps[$bits]=Join-Path $prefix "drive_c\tests$bits"
        $private[$bits]=Join-Path $prefix $(if($bits -eq 32){'drive_c\windows\SysWOW64'}else{'drive_c\windows\System32'})
        $builtins[$bits]=Join-Path $runtimeCopy "builtin\$arch"
        $custom[$bits]=Join-Path $prefix "drive_c\custom$bits";$empty[$bits]=Join-Path $prefix "drive_c\empty$bits"
        $isolatedChildren[$bits]=Join-Path $prefix "drive_c\children$bits"
        New-Item -ItemType Directory -Force -Path $apps[$bits],$private[$bits],$builtins[$bits],$custom[$bits],$empty[$bits],$isolatedChildren[$bits]|Out-Null
        foreach($name in @('native-startup-probe','native-startup-shadow-probe','native-startup-bridge-probe')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "$name$bits.exe") -Destination $apps[$bits]}
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-probe$bits.exe") -Destination $isolatedChildren[$bits]
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-private$bits.dll") -Destination (Join-Path $private[$bits] 'wined3d.dll')
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-private$bits.dll") -Destination (Join-Path $private[$bits] 'd3dcompiler_43.dll')
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-bridge$bits.dll") -Destination (Join-Path $private[$bits] 'winedows-startup-bridge.dll')
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-builtin$bits.dll") -Destination (Join-Path $builtins[$bits] 'wined3d.dll')
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-app$bits.dll") -Destination (Join-Path $custom[$bits] 'wined3d.dll')
    }
    foreach($bits in @(32,64)){
        $program="C:\tests$bits\native-startup-probe$bits.exe"
        $native=Join-Path $private[$bits] 'wined3d.dll';$builtin=Join-Path $builtins[$bits] 'wined3d.dll'
        Invoke-Startup "default-native-$bits" $bits $program 50524956 $native native ''
        Invoke-Startup "strict-native-$bits" $bits $program 50524956 $native native 'wined3d=n'
        Invoke-Startup "native-first-$bits" $bits $program 50524956 $native native 'wined3d=n,b'
        Invoke-Startup "builtin-first-$bits" $bits $program 4255494c $builtin builtin 'wined3d=b,n'
        $appDll=Join-Path $apps[$bits] 'wined3d.dll'
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-app$bits.dll") -Destination $appDll
        Invoke-Startup "app-local-precedence-$bits" $bits $program 4150504c $appDll native 'wined3d=n,b'
        Remove-Item -LiteralPath $appDll
        Invoke-Startup "transitive-private-$bits" $bits "C:\tests$bits\native-startup-bridge-probe$bits.exe" 50524956 $native native 'wined3d=n,b'
        if($hostCompilers[$bits]){
            Invoke-Startup "shadow-installed-host-dll-$bits" $bits "C:\tests$bits\native-startup-shadow-probe$bits.exe" 50524956 (Join-Path $private[$bits] 'd3dcompiler_43.dll') native 'd3dcompiler_43=n,b'
        }else{$cases.Add(@{fixture="shadow-installed-host-dll-$bits";status='unavailable';reason='Host lacks optional d3dcompiler_43.dll'})}
        foreach($child in @(32,64)){
            $childArgs=@('--child',"C:\tests$child\native-startup-probe$child.exe",'50524956',(Join-Path $private[$child] 'wined3d.dll'))
            Invoke-Startup "child-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n,b' $childArgs
            Invoke-Startup "custom-environment-child-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n,b' ($childArgs+@('--custom-environment'))
            Invoke-Startup "ansi-environment-child-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n,b' ($childArgs+@('--custom-environment','--ansi-child'))
            Invoke-Startup "suspended-child-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n,b' ($childArgs+@('--suspended'))
            $directoryArgs=@('--child',"C:\tests$child\native-startup-probe$child.exe",'4150504c',(Join-Path $custom[$child] 'wined3d.dll'),'--dll-directory',$custom[$child])
            Invoke-Startup "inherited-application-directory-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' $directoryArgs
            $logicalArgs=@('--child',"C:\tests$child\native-startup-probe$child.exe",'4150504c',(Join-Path $custom[$child] 'wined3d.dll'),'--dll-directory',"C:\custom$child")
            Invoke-Startup "logical-application-directory-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' $logicalArgs
            $systemArgs=@('--child',"C:\tests$child\native-startup-probe$child.exe",'50524956',(Join-Path $private[$child] 'wined3d.dll'),'--dll-directory',"$env:WINDIR\System32")
            Invoke-Startup "logical-system32-directory-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' $systemArgs
            $fallback=@('--child',"C:\tests$child\native-startup-probe$child.exe",'50524956',(Join-Path $private[$child] 'wined3d.dll'),'--dll-directory',$empty[$child])
            Invoke-Startup "custom-directory-private-fallback-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' $fallback
            Invoke-Startup "custom-directory-ansi-fallback-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' ($fallback+@('--custom-environment','--ansi-child'))
            Invoke-Startup "custom-directory-suspended-fallback-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' ($fallback+@('--suspended'))
            $bridge=@('--child',"C:\tests$child\native-startup-bridge-probe$child.exe",'50524956',(Join-Path $private[$child] 'wined3d.dll'),'--dll-directory',$empty[$child])
            Invoke-Startup "custom-directory-transitive-fallback-$bits-to-$child" $bits $program 50524956 $native native 'wined3d=n' $bridge
        }
        Invoke-Startup "unqualified-system-child-$bits" $bits $program 50524956 $native native 'wined3d=n,b' @('--system-child')
        # Keep the parent working with an app-local provider, but remove the
        # child's private-system native DLL. Its only native candidate is the
        # application's explicitly inherited directory, before builtin fallback.
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-app$bits.dll") -Destination $appDll
        try{
            foreach($child in @(32,64)){
                $childNative=Join-Path $private[$child] 'wined3d.dll'
                Remove-Item -LiteralPath $childNative
                try{
                    $customOnly=@('--child',"C:\children$child\native-startup-probe$child.exe",'4150504c',(Join-Path $custom[$child] 'wined3d.dll'),'--dll-directory',$custom[$child])
                    Invoke-Startup "custom-directory-only-native-first-$bits-to-$child" $bits $program 4150504c $appDll native 'wined3d=n,b' $customOnly
                    $logicalOnly=@('--child',"C:\children$child\native-startup-probe$child.exe",'4150504c',(Join-Path $custom[$child] 'wined3d.dll'),'--dll-directory',"C:\custom$child")
                    Invoke-Startup "logical-custom-only-native-first-$bits-to-$child" $bits $program 4150504c $appDll native 'wined3d=n,b' $logicalOnly
                    $systemFallback=@('--child',"C:\children$child\native-startup-probe$child.exe",'4255494c',(Join-Path $builtins[$child] 'wined3d.dll'),'--dll-directory',"$env:WINDIR\System32",'--child-kind','builtin')
                    Invoke-Startup "logical-system32-builtin-fallback-$bits-to-$child" $bits $program 4150504c $appDll native 'wined3d=n,b' $systemFallback
                }finally{Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-private$child.dll") -Destination $childNative}
            }
        }finally{Remove-Item -LiteralPath $appDll}
        foreach($loaderBits in @(32,64)){
            Invoke-Startup "loader-custom-directory-fallback-$loaderBits-to-$bits" $bits $program 50524956 $native native 'wined3d=n' -loaderBits $loaderBits -loaderDirectory $empty[$bits]
        }
        Remove-Item -LiteralPath $native
        Invoke-Startup "missing-native-builtin-fallback-$bits" $bits $program 4255494c $builtin builtin 'wined3d=n,b'
        Invoke-Startup "missing-strict-native-$bits" $bits $program 50524956 $native native 'wined3d=n' @() ([uint32]3221225781)
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "native-startup-private$bits.dll") -Destination $native
    }
    foreach($file in $tracked){if((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ne $before[$file]){throw "Original provider or fixture changed: $file"}}
    @{passed=$true;cases=$cases;originalFilesPreserved=$true;runtimeSource=$RuntimeDirectory;knownLimitations=@('Unsupported startup dependency plans preserve the application native search path with a diagnostic; full Wine loader behavior is not implemented.');utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Native startup suite passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;runtimeSource=$RuntimeDirectory;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}finally{[WinedowsStartupErrorMode]::SetErrorMode($oldErrorMode)|Out-Null}
$global:LASTEXITCODE=0
