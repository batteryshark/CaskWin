param([switch]$SkipBuild,[string]$RuntimeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){
    & "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory
    & "$PSScriptRoot\build-media-state-probe.ps1" -OutputDirectory $RuntimeDirectory
}
$session=Join-Path $PSScriptRoot ('test-output\media-state-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $session 'prefix';$fixtures=Join-Path $session 'fixtures'
New-Item -ItemType Directory -Path $session|Out-Null
python "$PSScriptRoot\make-cue-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'Media fixture generation failed'}
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
function Invoke-MediaState([string[]]$arguments,[string]$name,[string]$loader=$runtime){
    $saved=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{& $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $session "$name.log");$code=$LASTEXITCODE}
    finally{$ErrorActionPreference=$saved}
    if($code){throw "Media-state operation failed: $name (exit $code)"}
}
$sourceHashes=@{}
foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$sourceHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
Invoke-MediaState @('init') 'init'
$removable=Join-Path $prefix 'removable-e';$aliases=Join-Path $prefix 'alias-gh';$nested=Join-Path $prefix 'drive_c\nested-i'
$redirected=Join-Path $prefix 'redirected-j';$redirectTarget=Join-Path $prefix 'redirect-target'
New-Item -ItemType Directory -Path $removable,$aliases,$nested,$redirectTarget|Out-Null
$redirectFixture=$false
try{New-Item -ItemType Junction -Path $redirected -Target $redirectTarget|Out-Null;$redirectFixture=$true}
catch{Write-Host 'Redirected-root fixture skipped: local junction creation unavailable. Equal-root and nested-root checks remain mandatory.'}
[IO.File]::WriteAllText((Join-Path $removable 'state.txt'),'removable state fixture',[Text.Encoding]::ASCII)
Invoke-MediaState @('drive','mount','D',(Join-Path $fixtures 'disc.cue')) 'mount-d'
Invoke-MediaState @('drive','map','E',$removable,'removable','REMOVABLE_E') 'map-e'
Invoke-MediaState @('drive','map','G',$aliases,'cdrom','ALIAS_G') 'map-g'
Invoke-MediaState @('drive','map','H',$aliases,'cdrom','ALIAS_H') 'map-h'
Invoke-MediaState @('drive','map','I',$nested,'cdrom','NESTED_I') 'map-i'
if($redirectFixture){Invoke-MediaState @('drive','map','J',$redirected,'cdrom','REDIRECTED_J') 'map-j'}
$ini=Join-Path $prefix 'dosdevices\drives.ini';$mappingHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$mediaRoot=Join-Path $prefix '.winedows\media';$cacheHashes=@{}
foreach($file in Get-ChildItem -LiteralPath $mediaRoot -File -Recurse){$cacheHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$RuntimeDirectory\media-state-probe$bits.exe" -Destination $tests}
foreach($bits in @(32,64)){
    $other=if($bits -eq 32){64}else{32}
    Invoke-MediaState @('run',"C:\tests\media-state-probe$bits.exe",(Join-Path $tests "media-state-probe$other.exe"),(Join-Path $RuntimeDirectory "wine$bits.exe")) "state-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    Invoke-MediaState @('run',"C:\tests\media-state-probe$bits.exe",'--initial') "new-session-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $mappingHash){throw 'Eject/load changed persistent drive mapping'}
}
foreach($path in $sourceHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sourceHashes[$path]){throw "Original fixture changed: $path"}}
foreach($path in $cacheHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $cacheHashes[$path]){throw "Immutable cache changed: $path"}}
if([IO.File]::ReadAllText((Join-Path $removable 'state.txt')) -ne 'removable state fixture'){throw 'Ejected removable file bytes changed'}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
@{passed=$true;architectures=@('x86','x64');crossBitnessChildren=$true;customAnsiUnicodeEnvironments=$true;
    absentFileAndRawReads=$true;absentDirectoryEnumeration=$true;absentRemovableWrites=$true;
    absentMetadataFlushAndNewSections=$true;existingMappedPagesPreserved=$true;
    cddaReset=$true;removalLocks=$true;changeCounter=$true;concurrentTransitions=$true;sessionReset=$true;
    sessionLockPreserved=$true;aliasesReject=$true;redirectedRootFixture=$redirectFixture;configurationUnchanged=$true;originalsUnchanged=$true;
    cacheUnchanged=$true;hostDriveListUnchanged=$true;utc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $session 'result.json')
Write-Host "Media state suite passed: $session"
$global:LASTEXITCODE=0
