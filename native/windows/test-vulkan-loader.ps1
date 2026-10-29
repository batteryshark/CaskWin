param(
    [switch]$SkipBuild,
    [string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-06'),
    [string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\vulkan-loader-probes')
)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-vulkan-loader-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\vulkan-loader-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';$loader=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new();$failures=[Collections.Generic.List[string]]::new();$identities=@{}
$saved=$env:WINEDLLOVERRIDES
function Invoke-Case([string]$label,[string]$program,[string[]]$arguments,[switch]$Optional){
    $output=& $program @arguments 2>&1;$status=$LASTEXITCODE
    $output|Set-Content -LiteralPath (Join-Path $suite "$label.log")
    $unavailable=$Optional -and $status -eq 77
    $passed=$status -eq 0
    $cases.Add(@{case=$label;exitCode=$status;status=$(if($unavailable){'unavailable'}elseif($passed){'passed'}else{'failed'})})
    if(!$passed -and !$unavailable){$failures.Add($label);$output|Write-Host}
    Write-Host "$label exit=$status"
    return @{status=$status;output=$output}
}
function Accept([string]$label,[int]$bits,[string]$kind,[string]$shadow){
    $id=$identities[$bits]
    $null=Invoke-Case $label $loader @('--prefix',$prefix,'--desktop','off','run',"C:\$kind$bits\probe.exe",'--accept',$id[0],$id[1],$shadow)
}
try{
    $env:WINEDLLOVERRIDES=$null
    foreach($bits in @(32,64)){
        $baseline=Invoke-Case "native-baseline-$bits" (Join-Path $FixtureDirectory "vulkan-loader-probe$bits.exe") @('--native') -Optional
        if($baseline.status -eq 77){continue}
        if($baseline.status){throw 'Native Vulkan loader baseline failed'}
        $match=[regex]::Match(($baseline.output -join "`n"),'(?m)^HOST_ID ([0-9a-f]+) ([0-9a-f]{32})\s*$')
        if(!$match.Success){throw 'Native baseline did not report file identity'}
        $identities[$bits]=@($match.Groups[1].Value,$match.Groups[2].Value)
    }
    & $loader --prefix $prefix init *> (Join-Path $suite 'init.log')
    if($LASTEXITCODE){throw 'Vulkan loader prefix initialization failed'}
    foreach($bits in @(32,64)){
        if(!$identities.ContainsKey($bits)){continue}
        foreach($kind in @('clean','appmarker','appmalformed','local')){
            $directory=Join-Path $prefix "drive_c\$kind$bits"
            New-Item -ItemType Directory -Force -Path $directory|Out-Null
            Copy-Item -LiteralPath (Join-Path $FixtureDirectory "vulkan-loader-probe$bits.exe") -Destination (Join-Path $directory 'probe.exe')
            if($kind -in @('appmarker','local')){Copy-Item -LiteralPath (Join-Path $FixtureDirectory "vulkan-loader-marker$bits.dll") -Destination (Join-Path $directory 'vulkan-1.dll')}
            if($kind -eq 'appmalformed'){'Deliberately malformed private fixture; never a host DLL.'|Set-Content -LiteralPath (Join-Path $directory 'vulkan-1.dll')}
        }
        Accept "clean-fixed-host-$bits" $bits 'clean' 'clean'
        Accept "app-shadow-preloaded-$bits" $bits 'appmarker' 'marker'
        Accept "malformed-app-shadow-$bits" $bits 'appmalformed' 'clean'
        $system=if($bits -eq 32){'syswow64'}else{'system32'}
        $shadow=Join-Path $prefix "drive_c\windows\$system\vulkan-1.dll"
        New-Item -ItemType Directory -Force -Path (Split-Path $shadow)|Out-Null
        Copy-Item -LiteralPath (Join-Path $FixtureDirectory "vulkan-loader-marker$bits.dll") -Destination $shadow
        Accept "prefix-system-shadow-$bits" $bits 'clean' 'marker'
        Accept "app-and-prefix-shadows-$bits" $bits 'appmarker' 'marker'
        'Deliberately malformed private system fixture.'|Set-Content -LiteralPath $shadow
        Accept "malformed-prefix-shadow-$bits" $bits 'clean' 'clean'
        Remove-Item -LiteralPath $shadow
        $local=Join-Path $prefix "drive_c\local$bits"
        New-Item -ItemType File -Path (Join-Path $local 'probe.exe.local')|Out-Null
        $nativeLocal=Invoke-Case "native-dotlocal-$bits" (Join-Path $local 'probe.exe') @('--raw-local') -Optional
        if(!$nativeLocal.status){$null=Invoke-Case "redirected-fixed-host-rejected-$bits" $loader @('--prefix',$prefix,'run',"C:\local$bits\probe.exe",'--reject-local')}
    }
}catch{
    $failures.Add($_.Exception.Message)
    throw
}finally{
    $env:WINEDLLOVERRIDES=$saved
    @{passed=(!$failures.Count -and $identities.Count -gt 0);runtime=$RuntimeDirectory;cases=$cases;failures=$failures;
      hostArchitectures=@($identities.Keys);hostPolicyChanged=$false;gpuCreated=$false;
      coverage=@('independent native mapped-image file identity','fixed no-argument backend export','private application and system shadows','loaded marker reference retained','ordinary loading restored after backend call','malformed private shadows ignored by fixed backend');
      limitations=@('DotLocal rejection is tested only when native Windows applies it','File identity is checked after DLL initialization; this is not a security boundary','Vulkan rendering and Wine provider policy are separate tests');
      utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Vulkan loader results: $suite"
}
if($failures.Count -or !$identities.Count){throw 'Vulkan fixed host-loader boundary suite did not pass'}
$global:LASTEXITCODE=0
