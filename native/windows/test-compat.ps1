param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory}
$prefix=Join-Path $PSScriptRoot ('test-output\compat-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix init failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory | Out-Null
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$RuntimeDirectory\compat-probe$bits.exe" -Destination (Join-Path $directory "version$bits.exe")}
function Import-Version([string]$version,[string]$application=''){
    $key='HKEY_CURRENT_USER\Software\Wine'
    if($application){$key+="\AppDefaults\$application"}
    $file=Join-Path $prefix 'version.reg'
    [IO.File]::WriteAllText($file,"Windows Registry Editor Version 5.00`r`n`r`n[$key]`r`n`"Version`"=`"$version`"`r`n")
    & $runtime --prefix $prefix reg import $file
    if($LASTEXITCODE){throw 'Version import failed'}
}
function Assert-Native([int]$bits) {
    $hostOutput=& "$RuntimeDirectory\compat-probe$bits.exe"
    if($LASTEXITCODE){throw 'Host version query failed'}
    $output=& $runtime --prefix $prefix run "C:\tests\version$bits.exe"
    if($LASTEXITCODE){throw 'Native version query failed'}
    $expected=$hostOutput|Where-Object {$_ -like 'VERSION *'}
    $actual=$output|Where-Object {$_ -like 'VERSION *'}
    if(!$expected -or $expected -ne $actual){throw "Native version changed: $expected / $actual"}
    "PASS native version preserved ($bits-bit)"|Tee-Object -FilePath (Join-Path $prefix 'native.log') -Append
}
Import-Version 'native'
foreach($bits in @(32,64)){Assert-Native $bits}
$versions=@(
    @{name='win98';values=@('4','10','0x40a08ae','1','0')},
    @{name='winxp';values=@('5','1','2600','2','3')},
    @{name='win7';values=@('6','1','7601','2','1')},
    @{name='win10';values=@('10','0','19045','2','0')}
)
foreach($v in $versions){
    Import-Version $v.name
    foreach($bits in @(32,64)){
        & $runtime --prefix $prefix run "C:\tests\version$bits.exe" @($v.values) 2>&1 | Tee-Object -FilePath (Join-Path $prefix "$($v.name)-$bits.log")
        if($LASTEXITCODE){throw "$($v.name) failed ($bits-bit)"}
    }
}
Import-Version 'win98' 'version32.exe'
& $runtime --prefix $prefix run 'C:\tests\version32.exe' 4 10 0x40a08ae 1 0
if($LASTEXITCODE){throw 'Application version did not override prefix setting'}
Import-Version 'native' 'version64.exe'
Assert-Native 64
Import-Version 'not-a-wine-version' 'version64.exe'
& $runtime --prefix $prefix run 'C:\tests\version64.exe'
if(!$LASTEXITCODE){throw 'Invalid Wine version was silently accepted'}
@{passed=$true;architectures=@('x86','x64');versions=$versions.name;applicationOverride=$true;nativeOptOut=$true;invalidRejected=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
Write-Host "Compatibility tests passed: $prefix"
$global:LASTEXITCODE=0
