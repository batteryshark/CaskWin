param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\display-08\fixtures'),[string]$SdlPath,[switch]$SkipBuild,[switch]$BasicOnly)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-monitor-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\display-08\monitors-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output|Out-Null
$prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe';$results=[Collections.Generic.List[object]]::new()
function Invoke-MonitorCase([string]$Name,[string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $log=Join-Path $output "$Name.log"
    & $Program @Arguments 2>&1|Tee-Object -FilePath $log|Out-Host
    $code=$LASTEXITCODE;$results.Add(@{name=$Name;exitCode=$code;expected=$Expected;passed=($code -eq $Expected);log=$log})
}
$saved=$env:WINEDLLOVERRIDES;$sdlHash=$null
try {
    $env:WINEDLLOVERRIDES=$null
    foreach($bits in @(32,64)){
        Invoke-MonitorCase "native-$bits" "$FixtureDirectory\desktop-monitor-probe$bits.exe" @('--native')
        Invoke-MonitorCase "native-null-rect-$bits" "$FixtureDirectory\desktop-monitor-probe$bits.exe" @('--null-rect') 77
    }
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'Prefix initialization failed'}
    $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
    foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\desktop-monitor-probe$bits.exe" -Destination "$directory\monitor$bits.exe"}
    if($SdlPath){
        $SdlPath=[IO.Path]::GetFullPath($SdlPath);$sdlHash=(Get-FileHash -LiteralPath $SdlPath -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $SdlPath -Destination "$directory\game-SDL2.dll"
    }
    foreach($bits in @(32,64)){
        $base=@('--prefix',$prefix,'--desktop','Monitor,800x600','run',"C:\tests\monitor$bits.exe")
        $arguments=$base+@('--virtual');if(!$BasicOnly){$arguments+='--windows'}
        Invoke-MonitorCase "virtual-$bits" $runtime $arguments
        Invoke-MonitorCase "virtual-null-rect-$bits" $runtime ($base+@('--null-rect')) 77
        if(!$BasicOnly){foreach($child in @(32,64)){Invoke-MonitorCase "child-$bits-to-$child" $runtime ($base+@('--virtual','--child',"C:\tests\monitor$child.exe"))}}
    }
    if($SdlPath -and !$BasicOnly){Invoke-MonitorCase 'game-sdl64' $runtime @('--prefix',$prefix,'--desktop','Monitor,800x600','run','C:\tests\monitor64.exe','--virtual','--sdl','C:\tests\game-SDL2.dll')}
    Invoke-MonitorCase 'native-after64' "$FixtureDirectory\desktop-monitor-probe64.exe" @('--native')
    $before=Get-Content -LiteralPath "$output\native-64.log"|Where-Object {$_ -match '^(MONITOR|WORKAREA|HOST_MODE) '}
    $after=Get-Content -LiteralPath "$output\native-after64.log"|Where-Object {$_ -match '^(MONITOR|WORKAREA|HOST_MODE) '}
    $unchanged=!(Compare-Object $before $after)
    $sdlUnchanged=!$SdlPath -or (Get-FileHash -LiteralPath $SdlPath -Algorithm SHA256).Hash -eq $sdlHash
    $passed=$unchanged -and $sdlUnchanged -and !($results|Where-Object {!$_.passed})
    @{passed=$passed;runtime=$RuntimeDirectory;hostMonitorAndWorkAreaUnchanged=$unchanged;noInputOrHostModeset=$true;gameSdlSource=$SdlPath;gameSdlSha256=$sdlHash;gameSdlUnchanged=$sdlUnchanged;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$output\result.json"
    Write-Host "Desktop monitor results: $output"
    if(!$passed){throw "Desktop monitor regressions failed: $output"}
} finally {$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
