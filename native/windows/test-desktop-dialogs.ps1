param([switch]$SkipBuild,[string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'),[string]$FixtureDirectory)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$FixtureDirectory){$FixtureDirectory=$RuntimeDirectory}
$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-dialog-probe.ps1" -OutputDirectory $FixtureDirectory}
$prefix=Join-Path $PSScriptRoot ('test-output\desktop-dialogs-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\desktop-dialog-probe$bits.exe" -Destination "$directory\dialog$bits.exe"}
$before=& "$FixtureDirectory\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
if($LASTEXITCODE -or !$before){throw 'Host display query failed'}
$before|Set-Content -LiteralPath (Join-Path $prefix 'host-before.log')
$results=@()
foreach($bits in @(32,64)){
    foreach($mode in @('position','modal','modeless','message','nested','lifetime','controls','cross-child')){
        $log=Join-Path $prefix "$mode-$bits.log"
        $arguments=@('--prefix',$prefix,'--desktop','Dialogs,800x600','run',"C:\tests\dialog$bits.exe",$mode)
        if($mode -eq 'cross-child'){$other=if($bits -eq 32){64}else{32};$arguments+=,"C:\tests\dialog$other.exe"}
        & $runtime @arguments 2>&1|Tee-Object -FilePath $log
        $exit=$LASTEXITCODE
        $results+=@{architecture=$bits;case=$mode;exitCode=$exit;passed=($exit -eq 0);log=$log}
    }
}
$after=& "$FixtureDirectory\display-probe64.exe"|Where-Object {$_ -like 'DISPLAY *'}
$unchanged=$LASTEXITCODE -eq 0 -and $before -eq $after
$after|Set-Content -LiteralPath (Join-Path $prefix 'host-after.log')
$passed=$unchanged -and !($results|Where-Object {!$_.passed})
@{passed=$passed;runtime=$RuntimeDirectory;hostDisplayPreserved=$unchanged;cases=$results;noGlobalInput=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
if(!$passed){throw "Desktop dialog regression failed: $prefix"}
Write-Host "Desktop dialog tests passed: $prefix"
