param([switch]$SkipBuild,[string]$RuntimeDirectory='',[string]$ProbeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$ProbeDirectory){$ProbeDirectory=$RuntimeDirectory}
$ProbeDirectory=[IO.Path]::GetFullPath($ProbeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory;& "$PSScriptRoot\build-cdda-media-probe.ps1" -OutputDirectory $ProbeDirectory}
$prefix=Join-Path $PSScriptRoot ('test-output\cdda-media-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'CDDA media prefix initialization failed'}
$fixtures=Join-Path $prefix '.winedows\media\fixtures'
python "$PSScriptRoot\make-cdda-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'CDDA fixture generation failed'}
$manifest=Join-Path $fixtures 'valid\disc.ini'
$pcm=Join-Path $fixtures 'valid\track03.pcm'
$sourceHash=(Get-FileHash -LiteralPath $pcm -Algorithm SHA256).Hash
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
& $runtime --prefix $prefix drive audio D $manifest
if($LASTEXITCODE){throw 'CDDA media mount failed'}
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
foreach($bits in @(32,64)) {
    Copy-Item -LiteralPath "$ProbeDirectory\cdda-media-probe$bits.exe" -Destination $tests
    & "$RuntimeDirectory\wine$bits.exe" --prefix $prefix run "C:\tests\cdda-media-probe$bits.exe" $pcm 2>&1|Tee-Object -FilePath (Join-Path $prefix "raw-$bits.log")
    if($LASTEXITCODE){throw "CDDA raw probe $bits failed"}
}
$ini=Join-Path $prefix 'dosdevices\drives.ini'
$mappingHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$badCount=0
foreach($bad in Get-ChildItem -LiteralPath $fixtures -Directory -Filter 'bad-*') {
    $saved=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try {& $runtime --prefix $prefix drive audio D (Join-Path $bad.FullName 'disc.ini') 2>&1|Tee-Object -FilePath (Join-Path $prefix ($bad.Name+'.log'));$code=$LASTEXITCODE}
    finally {$ErrorActionPreference=$saved}
    if(!$code){throw "Malformed CDDA manifest accepted: $($bad.Name)"}
    if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $mappingHash){throw 'Malformed CDDA remount changed active mapping'}
    if($bad.Name -eq 'bad-msf-limit' -and (Get-Content -Raw -LiteralPath (Join-Path $prefix ($bad.Name+'.log'))) -notmatch 'representable MSF duration'){throw 'MSF boundary fixture failed for an unexpected reason'}
    $badCount++
}
if((Get-FileHash -LiteralPath $pcm -Algorithm SHA256).Hash -ne $sourceHash){throw 'PCM source was changed'}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
$folder=Join-Path $prefix 'drive_c\folder-disc';New-Item -ItemType Directory -Path $folder|Out-Null
& $runtime --prefix $prefix drive map D $folder cdrom
if($LASTEXITCODE){throw 'Folder remount failed'}
if([IO.File]::ReadAllText($ini) -match '(?m)^audio='){throw 'Folder remap retained stale CDDA mapping'}
& $runtime --prefix $prefix drive map D $folder cdrom D_DISC
& $runtime --prefix $prefix drive map E $folder cdrom E_DISC
& $runtime --prefix $prefix drive audio D $manifest
& $runtime --prefix $prefix drive audio E (Join-Path $fixtures 'alternate\disc.ini')
& $runtime --prefix $prefix drive audio F (Join-Path $fixtures 'boundary-valid\disc.ini')
if($LASTEXITCODE){throw 'Alias/boundary fixture setup failed'}
foreach($bits in @(32,64)) {
    foreach($mode in @('aliases','boundary')) {
        & "$RuntimeDirectory\wine$bits.exe" --prefix $prefix run "C:\tests\cdda-media-probe$bits.exe" "--$mode" 2>&1|Tee-Object -FilePath (Join-Path $prefix "$mode-$bits.log")
        if($LASTEXITCODE){throw "CDDA $mode probe $bits failed"}
    }
}
@{passed=$true;architectures=@('x86','x64');rawPcmCompared=$true;toc=$true;malformedManifests=$badCount;aliasedRawDrives=$true;duplicateAndReusedHandles=$true;msfBoundary=$true;sourceUnchanged=$true;hostDriveListUnchanged=$true;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
Write-Host "CDDA media suite passed: $prefix"
$global:LASTEXITCODE=0
