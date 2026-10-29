param([string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\dpi-08\fixtures'),[switch]$SkipBuild)
$ErrorActionPreference='Stop';$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-dpi-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\dpi-08\native-hosting-'+[Guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $output|Out-Null
$checks=[Collections.Generic.List[object]]::new()
foreach($parent in @('unaware-default','unaware-mixed','pm-default','pm-mixed')){foreach($bits in @(32,64)){foreach($child in @('unaware','pm')){
    $expected=if($child -eq 'pm'){'2'}else{'0'};$name="$parent-$child-$bits";$program=Join-Path $FixtureDirectory "desktop-dpi-$child$bits.exe"
    & "$FixtureDirectory\desktop-dpi-host64.exe" $parent $program $expected *> "$output\$name.log"
    $code=$LASTEXITCODE;$events=@(Get-Content -LiteralPath "$output\$name.log"|Where-Object {$_ -like '{*'}|ForEach-Object {$_|ConvertFrom-Json})
    $summary=$events|Where-Object {$_.kind -eq 'summary'}|Select-Object -Last 1
    $preserve=$parent -eq 'pm-mixed' -or ($parent -like 'unaware-*' -and $child -eq 'unaware') -or ($parent -eq 'pm-default' -and $child -eq 'pm')
    $expectedCode=if($preserve){0}else{2}
    $passed=$summary -and $summary.collectionErrors -eq 0 -and $code -eq $expectedCode
    $checks.Add(@{name=$name;exitCode=$code;expectedCode=$expectedCode;passed=$passed;summary=$summary;events=$events});Write-Host "$name exit=$code regressions=$($summary.regressions)"
}}}
$passed=!($checks|Where-Object {!$_.passed})
@{completed=$true;allExpectedOutcomes=$passed;checks=$checks;noInputCaptureOrModeset=$true;hostContextChangesWereThreadLocal=$true}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath "$output\result.json"
Write-Host "Native DPI hosting evidence: $output"
if(!$passed){throw "Native mixed DPI hosting observations differed from the expected matrix: $output"}
$global:LASTEXITCODE=0
