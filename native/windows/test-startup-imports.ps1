param([string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-imports-fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop'
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-startup-imports-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\startup-imports-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite | Out-Null
$cases=[Collections.Generic.List[object]]::new()
$hashes=@{}
Get-ChildItem -LiteralPath $FixtureDirectory -File | Where-Object {$_.Extension -in @('.exe','.dll')} | ForEach-Object {$hashes[$_.FullName]=(Get-FileHash -LiteralPath $_.FullName).Hash}
foreach($bits in @(32,64)){$path=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\d3dcompiler_43.dll'}else{'System32\d3dcompiler_43.dll'});if(Test-Path -LiteralPath $path){$hashes[$path]=(Get-FileHash -LiteralPath $path).Hash}}
function Invoke-Imports([string]$label,[int]$parent,[int]$bits,[string]$kind,[int]$count,[string]$value,[string]$file,[string]$custom,[string[]]$extra=@(),[string]$programOverride=''){
    $base=Join-Path $suite "layout$bits";$machine=if($bits -eq 32){'14c'}else{'8664'}
    $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $FixtureDirectory "startup-imports-probe$parent.exe"))
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $program=if($programOverride){$programOverride}else{"$base\app\startup-imports-$kind$bits.exe"}
    foreach($arg in @((Join-Path $FixtureDirectory "startup-imports-helper$parent.dll"),$program,$machine,"$base\private","$base\other",$custom,$value,$file,"$count")+$extra){$start.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    $finished=$process.WaitForExit(45000);if(!$finished){$process.Kill($true);$process.WaitForExit()}
    $code=$process.ExitCode;$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
    $name="$label-$parent-to-$bits";[IO.File]::WriteAllText((Join-Path $suite "$name.log"),$text)
    $passed=$finished -and $code -eq 0;$cases.Add(@{case=$name;parent=$parent;child=$bits;passed=$passed;exitCode=$code;plannedImports=$count})
    Write-Host "$(if($passed){'PASS'}else{'FAIL'}) $name"
    if(!$passed){Write-Output $text;throw "Startup imports fixture failed: $name"}
}
try{
    foreach($bits in @(32,64)){
        $base=Join-Path $suite "layout$bits";New-Item -ItemType Directory -Path "$base\app","$base\custom","$base\private","$base\other","$base\builtin" | Out-Null
        foreach($kind in @('direct','bridge','shadow','sxs-direct','sxs-bridge')){Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-$kind$bits.exe" -Destination "$base\app"}
        foreach($suffix in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-rename$suffix-$bits.exe" -Destination "$base\app"}
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$base\private\wined3d.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$base\private\d3dcompiler_43.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-bridge$bits.dll" -Destination "$base\private\winedows-startup-bridge.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-builtin$bits.dll" -Destination "$base\builtin\wined3d.dll"
    }
    foreach($parent in @(32,64)){foreach($bits in @(32,64)){
        $base=Join-Path $suite "layout$bits";$native="$base\private\wined3d.dll";$custom="$base\custom";$builtin="$base\builtin\wined3d.dll"
        Invoke-Imports direct $parent $bits direct 1 50524956 $native $custom
        Invoke-Imports transitive $parent $bits bridge 2 50524956 $native $custom
        Invoke-Imports private-over-host $parent $bits shadow 1 50524956 "$base\private\d3dcompiler_43.dll" $custom
        Invoke-Imports custom-environment $parent $bits bridge 2 50524956 $native $custom @('--custom-environment')
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$custom\wined3d.dll"
        Invoke-Imports custom-precedence $parent $bits direct 0 4150504c "$custom\wined3d.dll" $custom
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$base\app\wined3d.dll"
        Invoke-Imports app-precedence $parent $bits direct 0 4150504c "$base\app\wined3d.dll" $custom
        Remove-Item -LiteralPath "$base\app\wined3d.dll","$custom\wined3d.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-bridge$bits.dll" -Destination "$base\app\winedows-startup-bridge.dll"
        Invoke-Imports app-transitive $parent $bits bridge 1 50524956 $native $custom
        Remove-Item -LiteralPath "$base\app\winedows-startup-bridge.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-bridge$bits.dll" -Destination "$custom\winedows-startup-bridge.dll"
        Invoke-Imports custom-transitive $parent $bits bridge 1 50524956 $native $custom
        Remove-Item -LiteralPath "$custom\winedows-startup-bridge.dll"
        Invoke-Imports selected-builtin $parent $bits direct 0 4255494c $builtin $custom @('--builtin',$builtin)
        Invoke-Imports selected-builtin-dependency $parent $bits bridge 1 4255494c $builtin $custom @('--builtin',$builtin)
        $logical="C:\wd-startup-logical-$($suite.Substring($suite.Length-32))-$bits"
        if(Test-Path -LiteralPath $logical){throw 'Host directory collides with logical namespace fixture'}
        $mapping=@('--map-custom',$logical,$custom)
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$custom\wined3d.dll"
        Invoke-Imports logical-custom-overlay $parent $bits direct 1 4150504c "$custom\wined3d.dll" $logical $mapping
        Invoke-Imports logical-custom-environment $parent $bits direct 1 4150504c "$custom\wined3d.dll" $logical ($mapping+@('--custom-environment'))
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-bridge$bits.dll" -Destination "$custom\winedows-startup-bridge.dll"
        Invoke-Imports logical-custom-transitive $parent $bits bridge 2 4150504c "$custom\wined3d.dll" $logical $mapping
        Remove-Item -LiteralPath "$custom\winedows-startup-bridge.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$base\app\wined3d.dll"
        Invoke-Imports app-before-logical-overlay $parent $bits direct 0 50524956 "$base\app\wined3d.dll" $logical $mapping
        Remove-Item -LiteralPath "$base\app\wined3d.dll"
        Remove-Item -LiteralPath $native
        Invoke-Imports logical-custom-only $parent $bits direct 1 4150504c "$custom\wined3d.dll" $logical $mapping
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination $native
        Invoke-Imports builtin-before-logical-overlay $parent $bits direct 0 4255494c $builtin $logical ($mapping+@('--builtin',$builtin))
        Remove-Item -LiteralPath "$custom\wined3d.dll"
        Invoke-Imports logical-overlay-private-fallback $parent $bits direct 1 50524956 $native $logical $mapping
        Invoke-Imports logical-overlay-candidate-race $parent $bits direct 1 50524956 $native $logical ($mapping+@('--add-candidate',"$FixtureDirectory\native-startup-app$bits.dll","$custom\wined3d.dll"))
        Remove-Item -LiteralPath "$custom\wined3d.dll"
        $hostCustom="$base\host-custom";New-Item -ItemType Directory -Force -Path $hostCustom | Out-Null
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$hostCustom\wined3d.dll"
        Invoke-Imports mapped-custom-host-readthrough $parent $bits direct 0 4150504c "$hostCustom\wined3d.dll" $hostCustom @('--map-custom',$hostCustom,$custom)
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$custom\wined3d.dll"
        Invoke-Imports mapped-custom-overlay-before-host $parent $bits direct 1 50524956 "$custom\wined3d.dll" $hostCustom @('--map-custom',$hostCustom,$custom)
        Remove-Item -LiteralPath "$custom\wined3d.dll","$hostCustom\wined3d.dll"
        $assembly='Winedows.Startup.SideBySide';$assemblyDir=Join-Path "$base\app" $assembly
        New-Item -ItemType Directory -Force -Path $assemblyDir | Out-Null
        $processor=if($bits -eq 32){'x86'}else{'amd64'}
        $identity="<assemblyIdentity type=`"win32`" name=`"$assembly`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/>"
        [IO.File]::WriteAllText((Join-Path $assemblyDir "$assembly.manifest"),"<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`">$identity<file name=`"wined3d.dll`"/></assembly>")
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$assemblyDir\wined3d.dll"
        foreach($kind in @('direct','bridge')){
            $count=if($kind -eq 'bridge'){1}else{0}
            Invoke-Imports "side-by-side-$kind" $parent $bits "sxs-$kind" $count 50524956 "$assemblyDir\wined3d.dll" $custom
        }
        Invoke-Imports selected-builtin-side-by-side-direct $parent $bits sxs-direct 0 4255494c $builtin $custom @('--builtin',$builtin)
        Invoke-Imports selected-builtin-side-by-side-transitive-rejected $parent $bits sxs-bridge 0 4255494c $builtin $custom @('--builtin',$builtin,'--reject-plan')
        $manifest="$base\app\startup-imports-direct$bits.exe.manifest"
        [IO.File]::WriteAllText($manifest,"<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`">$identity</assembly>")
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$custom\wined3d.dll"
        Invoke-Imports ambiguous-manifest-native-preserved $parent $bits direct 0 4150504c "$custom\wined3d.dll" $custom @('--allow-unsupported')
        Remove-Item -LiteralPath "$custom\wined3d.dll"
        Remove-Item -LiteralPath $manifest
        Invoke-Imports relative-custom $parent $bits direct 1 50524956 $native '..\custom'
        Invoke-Imports drive-relative-native-only $parent $bits direct 0 50524956 $native 'C:custom' @('--unsupported-plan')
        Invoke-Imports root-relative-native-only $parent $bits direct 0 50524956 $native '\custom' @('--unsupported-plan')
        Invoke-Imports semicolon-native-rejection $parent $bits direct 0 50524956 $native "$custom;$base\private" @('--reject-directory')
        $semicolon=Join-Path $base 'app;literal';New-Item -ItemType Directory -Force -Path $semicolon | Out-Null
        Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-direct$bits.exe" -Destination $semicolon
        Invoke-Imports selected-builtin-semicolon-app $parent $bits direct 0 4255494c $builtin $custom @('--builtin',$builtin,'--clear-directory') "$semicolon\startup-imports-direct$bits.exe"
        Invoke-Imports semicolon-app-owned-directory $parent $bits direct 0 50524956 $native "$base\private" @() "$semicolon\startup-imports-direct$bits.exe"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$custom\wined3d.dll"
        Invoke-Imports semicolon-app-native-preserved $parent $bits direct 0 4150504c "$custom\wined3d.dll" $custom @('--allow-unsupported') "$semicolon\startup-imports-direct$bits.exe"
        Remove-Item -LiteralPath "$custom\wined3d.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-app$bits.dll" -Destination "$semicolon\wined3d.dll"
        Invoke-Imports empty-default-directory-fast-path $parent $bits direct 0 4150504c "$semicolon\wined3d.dll" $custom @('--clear-directory') "$semicolon\startup-imports-direct$bits.exe"
        Invoke-Imports empty-explicit-directory-fast-path $parent $bits direct 0 4150504c "$semicolon\wined3d.dll" $custom @('--empty-directory') "$semicolon\startup-imports-direct$bits.exe"
        Remove-Item -LiteralPath "$semicolon\wined3d.dll"
        Invoke-Imports custom-alias-of-private $parent $bits direct 0 50524956 $native "$base\private\."
        Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-direct$bits.exe" -Destination "$base\private"
        Invoke-Imports application-in-private-directory $parent $bits direct 0 50524956 $native $custom @() "$base\private\startup-imports-direct$bits.exe"
        Remove-Item -LiteralPath "$base\private\startup-imports-direct$bits.exe"
        if($parent -ne $bits){
            Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$base\private\wd-provider$parent.dll"
            Invoke-Imports cross-helper-rename-native-only $parent $bits direct 0 50524956 "$base\private\wd-provider$parent.dll" $custom @('--unsupported-plan') "$base\app\startup-imports-rename$parent-$bits.exe"
            Remove-Item -LiteralPath "$base\private\wd-provider$parent.dll"
        }
        Invoke-Imports missing-placeholder $parent $bits direct 1 50524956 $native $custom @('--omit-preload')
        Invoke-Imports directory-race $parent $bits direct 1 50524956 $native $custom @('--tamper-directory',"$base\other")
        Invoke-Imports absent-candidate-race $parent $bits direct 1 50524956 $native $custom @('--add-candidate',"$FixtureDirectory\native-startup-app$bits.dll","$custom\wined3d.dll")
        Remove-Item -LiteralPath "$custom\wined3d.dll"
        [IO.File]::WriteAllText("$base\private\kernel32.dll",'not a PE; KnownDLL must remain native')
        [IO.File]::WriteAllText("$base\private\unused-broken.dll",'not a PE; unused modules must remain unloaded')
        Invoke-Imports known-and-unused-excluded $parent $bits direct 1 50524956 $native $custom
        Remove-Item -LiteralPath "$base\private\kernel32.dll","$base\private\unused-broken.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-forward$bits.dll" -Destination $native
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination "$base\private\wd-startup-target.dll"
        Invoke-Imports forwarded-export $parent $bits direct 2 50524956 $native $custom
        Remove-Item -LiteralPath "$base\private\wd-startup-target.dll"
        Copy-Item -LiteralPath "$FixtureDirectory\startup-imports-cycle$bits.dll" -Destination $native
        Invoke-Imports cyclic-dependency-native-only $parent $bits direct 0 50524956 $native $custom @('--unsupported-plan')
        [IO.File]::WriteAllText($native,'not a PE')
        Invoke-Imports malformed-dependency-rejected $parent $bits direct 0 50524956 $native $custom @('--reject-plan')
        Invoke-Imports builtin-skips-malformed-native $parent $bits direct 0 4255494c $builtin $custom @('--builtin',$builtin)
        $other=if($bits -eq 32){64}else{32};Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$other.dll" -Destination $native
        Invoke-Imports wrong-architecture-rejected $parent $bits direct 0 50524956 $native $custom @('--reject-plan')
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-bridge$bits.dll" -Destination $native
        Invoke-Imports missing-export-rejected $parent $bits direct 0 50524956 $native $custom @('--reject-plan')
        Copy-Item -LiteralPath "$FixtureDirectory\native-startup-private$bits.dll" -Destination $native
        [IO.File]::WriteAllText("$base\app\startup-imports-direct$bits.exe.local",'')
        Invoke-Imports local-redirection-native-only $parent $bits direct 0 50524956 $native $custom @('--unsupported-plan')
        Remove-Item -LiteralPath "$base\app\startup-imports-direct$bits.exe.local"
    }}
    foreach($entry in $hashes.GetEnumerator()){if((Get-FileHash -LiteralPath $entry.Key).Hash -ne $entry.Value){throw "Original file changed: $($entry.Key)"}}
    @{passed=$true;cases=$cases;originalFilesPreserved=$true;productionIntegrated=$false;limitations=@('Planner traverses app/custom/private dependencies, leaving remaining host locations to Windows.','Unsupported planning preserves native launch with a diagnostic; invalid selected private providers remain errors.','Cross-architecture native paths containing the caller architecture followed by a dot remain native-only because upstream Detours would rename them.');utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$suite\result.json"
    Write-Host "Startup imports suite passed: $suite"
}catch{
    @{passed=$false;error=$_.Exception.Message;cases=$cases;productionIntegrated=$false;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$suite\result.json"
    throw
}
$global:LASTEXITCODE=0
