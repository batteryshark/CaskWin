param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-override-disabled-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\override-disabled-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new();$unavailable=[Collections.Generic.List[string]]::new()
$savedOrder=$env:WINEDLLOVERRIDES;$savedMarker=$env:WINEDOWS_OVERRIDE_MARKER
function Copy-Runtime([string]$name,[bool]$builtins){
 $folder=Join-Path $suite $name;New-Item -ItemType Directory -Path $folder|Out-Null
 foreach($file in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $file) -Destination $folder}
 if($builtins){foreach($arch in @('x86','x64')){
  $bundle=Join-Path $folder "builtin\$arch";New-Item -ItemType Directory -Path $bundle|Out-Null
  foreach($file in @('wined3d.dll','d3d9.dll','dxgi.dll','d3d11.dll','d3d10.dll','d3d10_1.dll','d3d10core.dll','d3dcompiler_39.dll')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory "builtin\$arch\$file") -Destination $bundle}
 }}
 return (Join-Path $folder 'wine64.exe')
}
function New-FixturePrefix([string]$name){
 $prefix=Join-Path $suite $name
 & $runtime --prefix $prefix init | Out-Host
 if($LASTEXITCODE){throw 'Cannot initialize override prefix'}
 foreach($bits in @(32,64)){
  $folder=Join-Path $prefix "drive_c\tests\$bits";New-Item -ItemType Directory -Path $folder|Out-Null
  Copy-Item -LiteralPath (Join-Path $FixtureDirectory "override-disabled-probe$bits.exe") -Destination (Join-Path $folder "probe$bits.exe")
  Copy-Item -LiteralPath (Join-Path $FixtureDirectory "override-disabled-static$bits.exe") -Destination (Join-Path $folder "static$bits.exe")
  Copy-Item -LiteralPath (Join-Path $FixtureDirectory "override-bridge$bits.dll") -Destination (Join-Path $folder 'override-bridge.dll')
  foreach($file in @('override-marker.dll','override-marker.dll.dll','override.marker.dll','wined3d.dll','d3d9.dll','xinput1_4.dll')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "override-marker$bits.dll") -Destination (Join-Path $folder $file)}
 }
 return $prefix
}
function Invoke-Case([string]$label,[int]$bits,[string]$order,[string[]]$arguments,[bool]$reject=$false,[string]$loader=$runtime,[string]$target=$prefix,[string]$program="C:\tests\$bits\probe$bits.exe",[bool]$executed=$false,[string]$diagnostic=''){
 $env:WINEDLLOVERRIDES=$order
 $marker=Join-Path $target 'drive_c\executed.bin';$env:WINEDOWS_OVERRIDE_MARKER=$marker
 if(Test-Path -LiteralPath $marker){Remove-Item -LiteralPath $marker}
 $log=Join-Path $suite "$label.log"
 & $loader --prefix $target run $program @arguments *> $log
 $code=$LASTEXITCODE;$content=Get-Content -LiteralPath $log -Raw
 $passed=if($reject){$code -ne 0}else{$code -eq 0}
 if($diagnostic){$passed=$passed -and $content -match $diagnostic}
 $didExecute=Test-Path -LiteralPath $marker;$passed=$passed -and ($didExecute -eq $executed)
 $cases.Add([pscustomobject]@{label=$label;bits=$bits;order=$order;expectedReject=$reject;exitCode=$code;entrypointExecuted=$didExecute;passed=$passed})
 Write-Host "$(if($passed){'PASS'}else{'FAIL'}) $label ($code, executed=$didExecute)"
}
function Import-Overrides([string]$name,[string]$content,[string]$target){
 $file=Join-Path $suite "$name.reg";[IO.File]::WriteAllText($file,$content,[Text.UTF8Encoding]::new($true))
 & $runtime --prefix $target reg import $file
 if($LASTEXITCODE){throw 'Cannot import override settings'}
}
try{
 $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_OVERRIDE_MARKER=$null
 $runtime=Copy-Runtime 'runtime' $true;$missing=Copy-Runtime 'runtime-no-builtins' $false
 $prefix=New-FixturePrefix 'primary'
 foreach($bits in @(32,64)){
  Invoke-Case "native-control-$bits" $bits '' @('--native','override-marker.dll') -executed $true
  foreach($order in @('override-marker=','override-marker=  ,  ','*override-marker=','override-marker=;*override-marker=')){
   $tag=if($order -eq 'override-marker='){'empty'}elseif($order -eq 'override-marker=  ,  '){'separators'}elseif($order -eq '*override-marker='){'wildcard'}else{'matching-aliases'}
   Invoke-Case "disabled-$tag-$bits" $bits $order @('--blocked','override-marker.dll')
  }
  Invoke-Case "extensionless-$bits" $bits 'override-marker=' @('--blocked','override-marker')
  Invoke-Case "mixed-case-$bits" $bits 'OVERRIDE-MARKER.DLL=' @('--blocked','OvErRiDe-MaRkEr.DlL')
  Invoke-Case "dotted-basename-$bits" $bits 'override.marker.dll=' @('--blocked','override.marker.dll')
  Invoke-Case "double-extension-disabled-$bits" $bits 'override-marker.dll.dll=' @('--blocked','override-marker.dll.dll')
  Invoke-Case "double-extension-independent-$bits" $bits 'override-marker.dll.dll=' @('--native','override-marker.dll') -executed $true
  Invoke-Case "resource-load-$bits" $bits 'override-marker=' @('--resource','override-marker.dll')
  Invoke-Case "transitive-load-$bits" $bits 'override-marker=' @('--blocked-dependency','override-bridge.dll')
  Invoke-Case "selectable-module-disabled-$bits" $bits 'wined3d=' @('--blocked','wined3d.dll')
  Invoke-Case "multiple-disabled-$bits" $bits 'override-marker,wined3d=' @('--blocked','override-marker.dll')
  Invoke-Case "trusted-existing-module-$bits" $bits 'xinput1_4=' @('--existing')
  Invoke-Case "startup-static-$bits" $bits 'override-marker=' @('--noop') -reject $true -program "C:\tests\$bits\static$bits.exe" -diagnostic 'disabled.*override-marker|override-marker.*disabled'
  Invoke-Case "builtin-first-$bits" $bits 'wined3d=b,n' @('--builtin','wined3d.dll')
  Invoke-Case "native-first-$bits" $bits 'wined3d=n,b' @('--native','wined3d.dll') -executed $true
  Invoke-Case "missing-builtin-fallback-$bits" $bits 'wined3d=b,n' @('--native','wined3d.dll') -loader $missing -executed $true
  Invoke-Case "missing-strict-builtin-$bits" $bits 'wined3d=b' @('--noop') -reject $true -loader $missing -diagnostic 'No available implementation'
  Invoke-Case "disabled-dependency-native-fallback-$bits" $bits 'd3d9=b,n;wined3d=' @('--native','d3d9.dll') -executed $true
  Invoke-Case "disabled-dependency-strict-builtin-$bits" $bits 'd3d9=b;wined3d=' @('--noop') -reject $true -diagnostic 'dependency is disabled.*wined3d'
  Invoke-Case "disabled-unused-native-family-member-$bits" $bits 'd3d10=;d3d11=n' @('--noop') -diagnostic 'DLL d3d10.dll: disabled'
  Invoke-Case "disabled-unused-builtin-family-member-$bits" $bits 'd3d10=;d3d11=b' @('--noop') -diagnostic 'DLL d3d11.dll: Wine builtin'
  foreach($order in @('kernel32=','api-ms-win-core-file-l1-1-0=','winedows32=','override-marker=;*override-marker=n','C:\tests\override-marker.dll=')){
   $label='unsupported-'+$bits+'-'+$cases.Count
   Invoke-Case $label $bits $order @('--noop') -reject $true -diagnostic 'unsupported|not supported'
  }
  # Native-first fallback is measurable only when the host lacks this Wine-only
  # DLL. The app-local file is moved within this disposable prefix, then restored.
  $system=if($bits -eq 32){Join-Path $env:WINDIR 'SysWOW64\wined3d.dll'}else{Join-Path $env:WINDIR 'System32\wined3d.dll'}
  if(Test-Path -LiteralPath $system){$unavailable.Add("native-first missing-native case ${bits}: host has wined3d.dll")}
  else{
   $file=Join-Path $prefix "drive_c\tests\$bits\wined3d.dll";$backup=$file+'.fixture-backup'
   Move-Item -LiteralPath $file -Destination $backup
   try{Invoke-Case "missing-native-fallback-$bits" $bits 'wined3d=n,b' @('--builtin','wined3d.dll')}
   finally{Move-Item -LiteralPath $backup -Destination $file}
  }
 }
 foreach($p in @(32,64)){foreach($c in @(32,64)){foreach($policy in @('inherit','custom','clear','native','startup','duplicate-disabled','duplicate-native')){
  $child=if($policy -eq 'startup'){"C:\tests\$c\static$c.exe"}else{"C:\tests\$c\probe$c.exe"}
  Invoke-Case "child-$p-to-$c-$policy" $p 'override-marker=' @('--child',$child,$policy) -executed ($policy -in @('clear','native','duplicate-native'))
 }}}
 $env:WINEDLLOVERRIDES=$null;$profile=New-FixturePrefix 'profiles'
 $reg=@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"override-marker"=""

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\probe32.exe\DllOverrides]
"override-marker"="native"
'@
 Import-Overrides 'profiles' $reg $profile
 Invoke-Case 'registry-default-empty-64' 64 '' @('--blocked','override-marker.dll') -target $profile
 Invoke-Case 'app-default-native-32' 32 '' @('--native','override-marker.dll') -target $profile -executed $true
 Invoke-Case 'environment-disables-app-native-32' 32 'override-marker=' @('--blocked','override-marker.dll') -target $profile
 Invoke-Case 'environment-enables-registry-disabled-64' 64 'override-marker=n' @('--native','override-marker.dll') -target $profile -executed $true
 $reg=@'
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"override-marker"="native"

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\probe32.exe\DllOverrides]
"override-marker"=""
'@
 Import-Overrides 'app-empty' $reg $profile
 Invoke-Case 'app-default-empty-32' 32 '' @('--blocked','override-marker.dll') -target $profile
 Invoke-Case 'registry-default-native-64' 64 '' @('--native','override-marker.dll') -target $profile -executed $true
 $failed=@($cases|Where-Object{!$_.passed})
 @{passed=($failed.Count -eq 0);cases=@($cases);unavailable=@($unavailable);runtime=$RuntimeDirectory;limitations=@('KnownDLL/core/API-set disabling and path-qualified override keys explicitly rejected','conflicting exact/wildcard values explicitly rejected because Wine precedence is path-sensitive','already loaded modules and fixed native hardware backends retain Wine/native loader reuse behavior');utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
 Write-Host "Disabled override evidence: $suite"
 if($failed.Count){throw "Disabled override suite failed: $($failed.label -join ', ')"}
}finally{$env:WINEDLLOVERRIDES=$savedOrder;$env:WINEDOWS_OVERRIDE_MARKER=$savedMarker}
$global:LASTEXITCODE=0
