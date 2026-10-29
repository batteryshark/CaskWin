param([ValidateSet('x86','x64','all')][string]$Arch='all',[switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-wine.ps1" -Arch $Arch}
$prefix=Join-Path $PSScriptRoot ('test-output\wine-graphics-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix initialization failed.'}
$arches=if($Arch -eq 'all'){@('x86','x64')}else{@($Arch)}
foreach($a in $arches){
    $bits=if($a -eq 'x86'){'32'}else{'64'}
    $target=Join-Path $prefix "drive_c\test-$a"
    New-Item -ItemType Directory -Path $target | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot\bin\wine-graphics-probe$bits.exe" -Destination (Join-Path $target 'probe.exe')
    & "$PSScriptRoot\use-wine-graphics.ps1" -Prefix $prefix -Program "C:\test-$a\probe.exe"
    & $runtime --prefix $prefix run "C:\test-$a\probe.exe" 2>&1 | Tee-Object -FilePath (Join-Path $prefix "result-$a.log")
    if($LASTEXITCODE){throw "Wine $a graphics probe failed. Artifacts: $prefix"}
    & "$PSScriptRoot\use-wine-graphics.ps1" -Prefix $prefix -Program "C:\test-$a\probe.exe" -Restore
    if(Test-Path -LiteralPath (Join-Path $target 'ddraw.dll')){throw 'DLL restoration failed.'}
    & $runtime --prefix $prefix run "C:\test-$a\probe.exe" --native 2>&1 | Tee-Object -FilePath (Join-Path $prefix "result-native-$a.log")
    if($LASTEXITCODE){throw "Native $a fallback probe failed. Artifacts: $prefix"}
}
@{passed=$true;wine='11.0';architectures=$arches;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $prefix 'result.json')
Write-Host "Wine graphics suite passed: $prefix"
