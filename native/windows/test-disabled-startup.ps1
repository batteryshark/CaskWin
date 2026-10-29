param([switch]$SkipBuild,[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\disabled-startup-fixtures'))
$ErrorActionPreference='Stop';$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-disabled-startup-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\disabled-startup-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new();$failed=$false
function Invoke-Graph([string]$label,[int]$parent,[int]$bits,[string]$kind,[string]$disabled,[string]$expect,[string]$custom,[string]$pathText,[string[]]$extra=@()){
    $base=Join-Path $suite "layout$bits";$machine=if($bits -eq 32){'14c'}else{'8664'}
    $arguments=@("$base\app\$kind.exe",$machine,"$base\private","$base\other",'-',$custom,"$base\app",$pathText,$disabled,$expect)+$extra
    $output=& (Join-Path $FixtureDirectory "disabled-startup-probe$parent.exe") @arguments 2>&1;$status=$LASTEXITCODE
    $name="$label-$parent-to-$bits";$output|Set-Content -LiteralPath (Join-Path $suite "$name.log")
    $cases.Add(@{case=$name;parent=$parent;child=$bits;passed=($status -eq 0);exitCode=$status})
    Write-Host "$name exit=$status";if($status){$output|Write-Host;throw "Disabled startup validation failed: $name"}
}
try{
    foreach($bits in @(32,64)){
        $base=Join-Path $suite "layout$bits";New-Item -ItemType Directory -Path "$base\app","$base\private","$base\other","$base\custom","$base\path","$base\builtin"|Out-Null
        foreach($kind in @('leaf','bridge','empty','api','ext')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "disabled-startup-$kind$bits.exe") -Destination "$base\app\$kind.exe"}
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory 'disabled-startup-managed.exe') -Destination "$base\app\managed.exe"
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-leaf$bits.dll") -Destination "$base\private\wd-disable-leaf.dll"
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-bridge$bits.dll") -Destination "$base\private\wd-disable-bridge.dll"
    }
    foreach($parent in @(32,64)){foreach($bits in @(32,64)){
        $base=Join-Path $suite "layout$bits";$private="$base\private";$custom="$base\custom";$nativePath="$base\path"
        Invoke-Graph native-direct $parent $bits leaf unrelated.dll allow $private '' @('--run')
        Invoke-Graph native-transitive $parent $bits bridge unrelated.dll allow $private '' @('--run')
        Invoke-Graph direct-disabled $parent $bits leaf WD-DISABLE-LEAF.dll reject $private ''
        Invoke-Graph transitive-private-disabled $parent $bits bridge wd-disable-leaf.dll reject $private ''
        Invoke-Graph direct-bridge-disabled $parent $bits bridge wd-disable-bridge.dll reject $private ''
        Invoke-Graph ordinary-core-imports $parent $bits empty unrelated.dll allow '-' '' @('--run')
        foreach($name in @('api','ext')){
            Copy-Item -LiteralPath "$private\wd-disable-bridge.dll" -Destination "$base\app\$name-helper.dll"
            Invoke-Graph "$name-prefix-transitive-disabled" $parent $bits $name wd-disable-leaf.dll reject $private ''
            Invoke-Graph "$name-prefix-native-baseline" $parent $bits $name unrelated.dll allow $private '' @('--run')
        }
        if($bits -eq 64){Invoke-Graph anycpu-native-startup $parent $bits managed unrelated.dll allow $private '' @('--run')}
        Copy-Item -LiteralPath "$private\wd-disable-leaf.dll" -Destination "$custom\wd-disable-leaf.dll"
        foreach($location in @('app','custom','path')){
            $target="$base\$location\wd-disable-bridge.dll";Move-Item -LiteralPath "$private\wd-disable-bridge.dll" -Destination $target
            try{
                Invoke-Graph "transitive-$location-disabled" $parent $bits bridge wd-disable-leaf.dll reject $custom $nativePath
                Invoke-Graph "transitive-$location-allowed" $parent $bits bridge unrelated.dll allow $custom $nativePath
            }finally{Move-Item -LiteralPath $target -Destination "$private\wd-disable-bridge.dll"}
        }
        Remove-Item -LiteralPath "$custom\wd-disable-leaf.dll"
        foreach($kind in @('forward','ordinal')){
            Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-$kind$bits.dll") -Destination "$private\wd-disable-bridge.dll" -Force
            Invoke-Graph "$kind-disabled" $parent $bits bridge wd-disable-leaf.dll reject $private ''
            Invoke-Graph "$kind-native" $parent $bits bridge unrelated.dll allow $private '' @('--run')
        }
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-contract$bits.dll") -Destination "$private\wd-disable-bridge.dll" -Force
        Invoke-Graph real-api-contract-host $parent $bits bridge unrelated.dll allow $private ''
        Invoke-Graph real-api-contract-disabled-host $parent $bits bridge kernel32.dll reject $private ''
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-unknown-contract$bits.dll") -Destination "$private\wd-disable-bridge.dll" -Force
        Invoke-Graph unknown-api-contract-fails $parent $bits bridge unrelated.dll reject $private ''
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "wd-disable-bridge$bits.dll") -Destination "$private\wd-disable-bridge.dll" -Force
        Copy-Item -LiteralPath "$private\wd-disable-bridge.dll" -Destination "$base\builtin\wd-disable-bridge.dll" -Force
        $preload=@('--preload',"$base\builtin\wd-disable-bridge.dll")
        Invoke-Graph preload-transitive-disabled $parent $bits empty wd-disable-leaf.dll reject $private '' $preload
        Invoke-Graph preload-itself-disabled $parent $bits empty wd-disable-bridge.dll reject $private '' $preload
        Invoke-Graph preload-unused-allowed $parent $bits empty unrelated.dll allow $private '' $preload
        'Not a valid executable image'|Set-Content -LiteralPath "$base\app\wd-disable-bridge.dll"
        Invoke-Graph explicit-preload-before-app $parent $bits bridge unrelated.dll allow $private '' $preload
        Invoke-Graph malformed-app-dependency $parent $bits bridge unrelated.dll reject $private ''
        Remove-Item -LiteralPath "$base\app\wd-disable-bridge.dll"
        Invoke-Graph higher-candidate-race $parent $bits leaf unrelated.dll allow $private '' @('--copy-candidate',"$private\wd-disable-leaf.dll","$base\app\wd-disable-leaf.dll")
        Remove-Item -LiteralPath "$base\app\wd-disable-leaf.dll"
        Invoke-Graph inherited-directory-race $parent $bits leaf unrelated.dll allow $private '' @('--change-directory',"$base\other")
        New-Item -ItemType File -Path "$base\app\leaf.exe.local"|Out-Null
        Invoke-Graph unsupported-dotlocal $parent $bits leaf unrelated.dll reject $private ''
        Invoke-Graph policy-off-preserves-native $parent $bits leaf - allow $private ''
        Remove-Item -LiteralPath "$base\app\leaf.exe.local"
        Move-Item -LiteralPath "$private\wd-disable-leaf.dll" -Destination "$base\other\wd-disable-leaf.dll"
        Invoke-Graph unresolved-noncore-fails $parent $bits leaf unrelated.dll reject $private ''
        Invoke-Graph missing-disabled-direct $parent $bits leaf wd-disable-leaf.dll reject $private ''
        Move-Item -LiteralPath "$base\other\wd-disable-leaf.dll" -Destination "$private\wd-disable-leaf.dll"
    }}
}catch{$failed=$true;throw}finally{
    @{passed=(!$failed);cases=$cases;runtimeModified=$false;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Disabled startup results: $suite"
}
$global:LASTEXITCODE=0
