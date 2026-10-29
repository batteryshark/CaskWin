param([switch]$SkipBuild,[string]$RuntimeDirectory='',[string]$ProbeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$ProbeDirectory){$ProbeDirectory=$RuntimeDirectory}
$ProbeDirectory=[IO.Path]::GetFullPath($ProbeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory;& "$PSScriptRoot\build-media-probe.ps1" -OutputDirectory $ProbeDirectory}
$session=Join-Path $PSScriptRoot ('test-output\media-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $session 'prefix'
$fixtures=Join-Path $session 'fixtures'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Path $session|Out-Null
python "$PSScriptRoot\make-media-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'Media fixture generation failed'}
$manifest=Get-Content -Raw -LiteralPath (Join-Path $fixtures 'manifest.json')|ConvertFrom-Json
$physicalDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
function Invoke-Media([string[]]$arguments,[string]$name,[switch]$ExpectFailure,[string]$loader=$runtime) {
    $oldPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try {
        & $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $session "$name.log")
        $code=$LASTEXITCODE
    } finally {$ErrorActionPreference=$oldPreference}
    if($ExpectFailure){if(!$code){throw "Malformed/invalid media operation succeeded: $name"}}
    elseif($code){throw "Media fixture failed: $name (exit $code)"}
}
function Read-Mapping {
    $section='';$mapping=@{}
    foreach($line in [IO.File]::ReadAllLines((Join-Path $prefix 'dosdevices\drives.ini'))) {
        if($line -match '^\[(.+)\]$'){$section=$Matches[1]}
        elseif($section -eq 'd' -and $line -match '^([^=]+)=(.*)$'){$mapping[$Matches[1]]=$Matches[2]}
    }
    foreach($key in @('path','image')){if($mapping[$key]){$mapping[$key]=Join-Path $prefix $mapping[$key]}}
    return $mapping
}
function Assert-Hash([string]$path,[string]$expected) {
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected){throw "Unexpected media modification: $path"}
}
Invoke-Media @('init') 'init'
$tests=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $tests|Out-Null
foreach($bits in @(32,64)){Copy-Item -LiteralPath "$ProbeDirectory\media-probe$bits.exe" -Destination $tests}
$savedOverrides=$env:WINEDLLOVERRIDES
try {
    # This suite exercises media with the ordinary loader defaults.
    Remove-Item Env:WINEDLLOVERRIDES -ErrorAction SilentlyContinue
    foreach($kind in @('primary','joliet')) {
        $disc=$manifest.$kind
        Invoke-Media @('drive','mount','D',$disc.path) "mount-$kind"
        $mapping=Read-Mapping
        if($mapping.label -ne $disc.label -or [uint32]$mapping.serial -ne [uint32]$disc.serial){throw 'Image label/serial did not persist'}
        if([uint64]$mapping.volume_size -ne $disc.size -or [int]$mapping.sector_size -ne 2048){throw 'Image geometry did not persist'}
        Assert-Hash $mapping.image $disc.sha256
        foreach($bits in @(32,64)) {
            $loader=Join-Path $RuntimeDirectory "wine$bits.exe"
            Invoke-Media @('run',"C:\tests\media-probe$bits.exe",$disc.label,[string]$disc.serial,[string]$disc.size,$mapping.path,$mapping.image,$kind.ToUpperInvariant()) "$kind-$bits" -loader $loader
        }
        Assert-Hash $disc.path $disc.sha256
        Assert-Hash $mapping.image $disc.sha256
    }
    $ini=Join-Path $prefix 'dosdevices\drives.ini'
    $mappingHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
    $cacheCount=@(Get-ChildItem -LiteralPath (Join-Path $prefix '.winedows\media') -Directory).Count
    $index=0
    foreach($bad in Get-ChildItem -LiteralPath $fixtures -Filter 'bad-*.iso') {
        $before=(Get-FileHash -LiteralPath $bad.FullName -Algorithm SHA256).Hash
        $loader=Join-Path $RuntimeDirectory $(if($index++%2){'wine32.exe'}else{'wine64.exe'})
        Invoke-Media @('drive','mount','D',$bad.FullName) $bad.BaseName -ExpectFailure -loader $loader
        Assert-Hash $ini $mappingHash
        Assert-Hash $bad.FullName $before
        if($bad.BaseName -eq 'bad-aggregate-metadata' -and (Get-Content -Raw -LiteralPath (Join-Path $session ($bad.BaseName+'.log'))) -notmatch 'aggregate filesystem metadata') {throw 'Aggregate metadata fixture failed for an unexpected reason'}
    }
    if(@(Get-ChildItem -LiteralPath (Join-Path $prefix '.winedows\media') -Directory).Count -ne $cacheCount){throw 'Malformed images created partial caches'}
    Invoke-Media @('drive','mount','C',$manifest.primary.path) 'protected-c' -ExpectFailure
    Assert-Hash $ini $mappingHash
    # A retained snapshot remains usable when the original source is unavailable.
    $source=$manifest.joliet.path;$offline=$source+'.offline'
    Move-Item -LiteralPath $source -Destination $offline
    try {
        $disc=$manifest.joliet
        Invoke-Media @('run','C:\tests\media-probe64.exe',$disc.label,[string]$disc.serial,[string]$disc.size,$mapping.path,$mapping.image,'JOLIET') 'original-source-offline'
    } finally {Move-Item -LiteralPath $offline -Destination $source}
    Invoke-Media @('drive','mount','D',$manifest.primary.path,'CUSTOM_DISC') 'override-label'
    $custom=Read-Mapping
    if($custom.label -ne 'CUSTOM_DISC' -or [uint32]$custom.serial -ne [uint32]$manifest.primary.serial){throw 'Custom label changed the image serial'}
    $disc=$manifest.primary
    Invoke-Media @('run','C:\tests\media-probe64.exe','CUSTOM_DISC',[string]$disc.serial,[string]$disc.size,$custom.path,$custom.image,'PRIMARY') 'custom-label-probe'
    Invoke-Media @('drive','map','E',$custom.path,'cdrom','ISO_FILES_ALIAS') 'iso-files-alias'
    foreach($bits in @(32,64)) {
        Invoke-Media @('run',"C:\tests\media-probe$bits.exe",'--alias-only') "iso-alias-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    }
    # Hand-editing two image fields to one snapshot is rejected before launch;
    # drive mount remains available to replace the alias with a unique snapshot.
    $beforeAlias=[IO.File]::ReadAllBytes($ini)
    $relativeFiles=$custom.path.Substring($prefix.Length+1);$relativeImage=$custom.image.Substring($prefix.Length+1)
    [IO.File]::AppendAllText($ini,"`r`n[f]`r`npath=$relativeFiles`r`nimage=$relativeImage`r`ntype=5`r`nreadonly=1`r`n",[Text.Encoding]::Unicode)
    try {
        Invoke-Media @('run','C:\tests\media-probe64.exe','--alias-only') 'duplicate-image-rejected' -ExpectFailure
        if((Get-Content -Raw -LiteralPath (Join-Path $session 'duplicate-image-rejected.log')) -notmatch 'share one ISO snapshot'){throw 'Duplicate image fixture failed for an unexpected reason'}
        Invoke-Media @('drive','mount','F',$manifest.primary.path) 'repair-image-alias'
        Invoke-Media @('run','C:\tests\media-probe64.exe','--alias-only') 'repaired-image-alias'
    } catch {[IO.File]::WriteAllBytes($ini,$beforeAlias);throw}
    $folder=Join-Path $prefix 'drive_c\folder-disc';New-Item -ItemType Directory -Path $folder|Out-Null
    Invoke-Media @('drive','map','D',$folder,'cdrom','FOLDER_DISC') 'replace-with-folder'
    $folderMapping=Read-Mapping
    foreach($key in @('image','image_size','volume_size','sector_size','serial')) {
        if($folderMapping.ContainsKey($key)){throw "Folder remapping retained stale $key"}
    }
    foreach($kind in @('primary','joliet')){Assert-Hash $manifest.$kind.path $manifest.$kind.sha256}
    $after=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
    if(Compare-Object $physicalDrives $after){throw 'Host drive list changed during media test'}
    @{
        passed=$true;architectures=@('x86','x64');formats=@('ISO9660','Joliet');malformedFixtures=$index;
        rawSectorReads=$true;sourceUnchanged=$true;independentSnapshot=$true;hostDriveListUnchanged=$true;
        readonlyCache=$true;atomicRemount=$true;aggregateMetadataLimit=$true;rawDriveAliases=$true;duplicateImageRejected=$true;utc=[DateTime]::UtcNow.ToString('o')
    }|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $session 'result.json')
    Write-Host "Media suite passed: $session"
} finally {$env:WINEDLLOVERRIDES=$savedOverrides}
$global:LASTEXITCODE=0
