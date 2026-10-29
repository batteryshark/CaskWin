param([switch]$SkipBuild,[string]$RuntimeDirectory='',[string]$ProbeDirectory='')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$ProbeDirectory){$ProbeDirectory=$RuntimeDirectory}
$ProbeDirectory=[IO.Path]::GetFullPath($ProbeDirectory)
if(!$SkipBuild){
    & "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory
    & "$PSScriptRoot\build-cue-media-probe.ps1" -OutputDirectory $ProbeDirectory
}
$session=Join-Path $PSScriptRoot ('test-output\cue-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $session 'prefix';$fixtures=Join-Path $session 'fixtures'
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
New-Item -ItemType Directory -Path $session|Out-Null
python "$PSScriptRoot\make-cue-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'CUE fixture generation failed'}
$fixture=Get-Content -Raw -LiteralPath (Join-Path $fixtures 'fixture.json')|ConvertFrom-Json
Copy-Item -LiteralPath $fixture.bin -Destination (Join-Path $session 'outside.bin')
$sourceHashes=@{}
foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$sourceHashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
function Invoke-Cue([string[]]$arguments,[string]$name,[switch]$ExpectFailure,[string]$loader=$runtime) {
    $saved=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try {& $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $session "$name.log");$code=$LASTEXITCODE}
    finally {$ErrorActionPreference=$saved}
    if($ExpectFailure){if(!$code){throw "Invalid CUE operation succeeded: $name"}}
    elseif($code){throw "CUE operation failed: $name (exit $code)"}
}
function Read-CueMapping {
    $section='';$values=@{}
    foreach($line in [IO.File]::ReadAllLines((Join-Path $prefix 'dosdevices\drives.ini'))) {
        if($line -match '^\[(.+)\]$'){$section=$Matches[1]}
        elseif($section -eq 'd' -and $line -match '^([^=]+)=(.*)$'){$values[$Matches[1]]=$Matches[2]}
    }
    foreach($key in @('path','image','audio','cue','bin')){if($values[$key]){$values[$key]=Join-Path $prefix $values[$key]}}
    return $values
}
Invoke-Cue @('init') 'init'
Invoke-Cue @('drive','mount','D',$fixture.cue) 'mount-cue'
$mapping=Read-CueMapping
if([uint64]$mapping.image_size -ne 64*2048 -or [uint64]$mapping.volume_size -ne 48*2048 -or [int]$mapping.sector_size -ne 2048){throw 'CUE data/filesystem geometry was conflated'}
$cache=Split-Path $mapping.audio
$retainedCue=if($mapping.cue){$mapping.cue}else{Join-Path $cache 'source.cue'}
$retainedBin=if($mapping.bin){$mapping.bin}else{Join-Path $cache 'disc.bin'}
$privateFiles=@($retainedCue,$retainedBin,$mapping.image,$mapping.audio,(Join-Path $cache 'track02.pcm'),(Join-Path $cache 'track03.pcm'))
if((Get-FileHash -LiteralPath $retainedCue -Algorithm SHA256).Hash -ne $fixture.cue_sha256 -or
   (Get-FileHash -LiteralPath $retainedBin -Algorithm SHA256).Hash -ne $fixture.bin_sha256){throw 'Private CUE/BIN snapshot differs from original'}
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
foreach($bits in @(32,64)) {
    Copy-Item -LiteralPath "$ProbeDirectory\cue-media-probe$bits.exe" -Destination $tests
    Invoke-Cue (@('run',"C:\tests\cue-media-probe$bits.exe")+$privateFiles) "mixed-mode-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
}
$ini=Join-Path $prefix 'dosdevices\drives.ini';$mappingHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$badCount=0
foreach($bad in Get-ChildItem -LiteralPath $fixtures -Filter 'bad-*.cue') {
    foreach($bits in @(32,64)) {
        $loader=Join-Path $RuntimeDirectory "wine$bits.exe";$logName=$bad.BaseName+"-$bits"
        Invoke-Cue @('drive','mount','D',$bad.FullName) $logName -ExpectFailure -loader $loader
        if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $mappingHash){throw 'Malformed CUE changed the previous mapping'}
        if($bad.BaseName -in @('bad-traversal','bad-absolute-file') -and (Get-Content -Raw -LiteralPath (Join-Path $session ($logName+'.log'))) -notmatch 'without traversal'){throw 'CUE traversal fixture failed for the wrong reason'}
    }
    $badCount++
}
# The v2 layout validator also protects explicit drive-audio imports.
$metadata=[IO.File]::ReadAllText($mapping.audio)
$badLayouts=@{
    'extent'=$metadata.Replace('extent_start=64','extent_start=63');
    'index'=$metadata.Replace('start=69','start=500');
    'data-size'=$metadata.Replace('sectors=64','sectors=63');
    'origin'=$metadata.Replace('data_source=cue','data_source=iso')
}
$layoutErrors=@{
    'extent'='track indexes do not match';'index'='track indexes do not match';
    'data-size'='data extent does not match';'origin'='manifest version and data source disagree'
}
foreach($name in $badLayouts.Keys) {
    $badDir=Join-Path $prefix ".winedows\media\bad-cue-$name";New-Item -ItemType Directory -Path $badDir|Out-Null
    Copy-Item -LiteralPath (Join-Path $cache 'track02.pcm'),(Join-Path $cache 'track03.pcm') -Destination $badDir
    $badManifest=Join-Path $badDir 'disc.ini';[IO.File]::WriteAllText($badManifest,$badLayouts[$name],[Text.Encoding]::ASCII)
    foreach($bits in @(32,64)) {
        Invoke-Cue @('drive','audio','D',$badManifest) "bad-layout-$name-$bits" -ExpectFailure -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
        if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $mappingHash){throw 'Malformed CUE layout changed the previous mapping'}
        if((Get-Content -Raw -LiteralPath (Join-Path $session "bad-layout-$name-$bits.log")) -notmatch $layoutErrors[$name]){throw "Malformed CUE layout failed for the wrong reason: $name"}
    }
}
Move-Item -LiteralPath $fixture.cue -Destination ($fixture.cue+'.offline')
Move-Item -LiteralPath $fixture.bin -Destination ($fixture.bin+'.offline')
try {Invoke-Cue (@('run','C:\tests\cue-media-probe64.exe')+$privateFiles) 'originals-offline'}
finally {
    Move-Item -LiteralPath ($fixture.cue+'.offline') -Destination $fixture.cue
    Move-Item -LiteralPath ($fixture.bin+'.offline') -Destination $fixture.bin
}
foreach($path in $sourceHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sourceHashes[$path]){throw "Original fixture changed: $path"}}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
@{passed=$true;architectures=@('x86','x64');mode='single-file MODE1/2352 plus AUDIO';storedIndex00=$true;insertedPregap=$true;
    toc=$true;rawData=$true;rawAudioCompared=$true;malformedCue=$badCount;malformedLayouts=$badLayouts.Count;
    atomicRollback=$true;privateCacheWriteProtected=$true;originalsUnchanged=$true;offlineOriginals=$true;hostDriveListUnchanged=$true;utc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $session 'result.json')
Write-Host "CUE media suite passed: $session"
$global:LASTEXITCODE=0
