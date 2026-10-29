param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$MarkerDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\disabled-alias-fixtures'))
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build-disabled-alias-probe.ps1" -OutputDirectory $FixtureDirectory}
$runtime=Join-Path ([IO.Path]::GetFullPath($RuntimeDirectory)) 'wine64.exe'
$suite=Join-Path $PSScriptRoot ('test-output\disabled-alias-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $suite|Out-Null
$prefix=Join-Path $suite 'prefix';$cases=[Collections.Generic.List[object]]::new();$unavailable=[Collections.Generic.List[object]]::new()
$savedOverrides=$env:WINEDLLOVERRIDES;$savedMarker=$env:WINEDOWS_OVERRIDE_MARKER;$failed=$false
try{
    $env:WINEDLLOVERRIDES=$null;$env:WINEDOWS_OVERRIDE_MARKER=$null
    & $runtime --prefix $prefix init *> (Join-Path $suite 'init.log');if($LASTEXITCODE){throw 'Cannot initialize alias prefix'}
    foreach($bits in @(32,64)){foreach($kind in @('hardlink','symlink')){
        $folder=Join-Path $prefix "drive_c\aliases\$bits-$kind";New-Item -ItemType Directory -Path $folder|Out-Null
        $probe=Join-Path $folder "probe$bits.exe";Copy-Item -LiteralPath (Join-Path $FixtureDirectory "disabled-alias-probe$bits.exe") -Destination $probe
        Copy-Item -LiteralPath (Join-Path $MarkerDirectory "override-marker$bits.dll") -Destination (Join-Path $folder 'allowed-target.dll')
        Copy-Item -LiteralPath (Join-Path $MarkerDirectory "override-bridge$bits.dll") -Destination (Join-Path $folder 'override-bridge.dll')
        $target=if($kind -eq 'symlink'){'allowed-target.dll'}else{Join-Path $folder 'allowed-target.dll'}
        $linkLog=Join-Path $suite "link-$bits-$kind.log"
        & $probe --make-link $kind (Join-Path $folder 'override-marker.dll') $target *> $linkLog;$linkCode=$LASTEXITCODE
        if($linkCode){$unavailable.Add(@{bits=$bits;kind=$kind;exitCode=$linkCode;reason=(Get-Content -LiteralPath $linkLog -Raw)});Write-Host "UNVERIFIED $bits $kind (link unavailable)";continue}
        $nativeDuplicateSignature=$null
        foreach($mode in @('allowed','blocked')){foreach($action in @('probe','sections')){
            $label="$bits-$kind-$mode-$action";$env:WINEDLLOVERRIDES=if($mode -eq 'blocked'){'override-marker='}else{$null}
            $marker=Join-Path $folder "$mode-$action-executed.bin";$env:WINEDOWS_OVERRIDE_MARKER=$marker;$log=Join-Path $suite "$label.log"
            & $runtime --prefix $prefix run "C:\aliases\$bits-$kind\probe$bits.exe" "--$action" $mode *> $log;$code=$LASTEXITCODE
            $executed=Test-Path -LiteralPath $marker;$passed=$code -eq 0 -and ($executed -eq ($mode -eq 'allowed' -and $action -eq 'probe'))
            if($action -eq 'sections'){
                $signature=@(Get-Content -LiteralPath $log|Where-Object{$_ -match '^DUPLICATE_FAIL '}) -join "`n"
                if($mode -eq 'allowed'){$nativeDuplicateSignature=$signature}else{$passed=$passed -and $signature -eq $nativeDuplicateSignature}
            }
            $cases.Add(@{case=$label;bits=$bits;kind=$kind;mode=$mode;action=$action;exitCode=$code;entrypointExecuted=$executed;passed=$passed;log=$log})
            Write-Host "$(if($passed){'PASS'}else{'FAIL'}) $label (exit=$code, entrypoint=$executed)"
            if(!$passed){$failed=$true;Get-Content -LiteralPath $log|Write-Host}
        }}
    }}
}catch{$failed=$true;throw}finally{
    $env:WINEDLLOVERRIDES=$savedOverrides;$env:WINEDOWS_OVERRIDE_MARKER=$savedMarker
    @{passed=(!$failed);fullyVerified=(!$failed -and $unavailable.Count -eq 0);cases=@($cases);unavailable=@($unavailable);runtime=$RuntimeDirectory;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Alias evidence: $suite"
}
if($failed){throw 'Dynamic transitive alias verification failed'}
$global:LASTEXITCODE=0
