param([switch]$SkipBuild,[string]$RuntimeDirectory='',[string]$ProbeDirectory='',[string]$SevenZip='C:\Program Files\7-Zip\7z.exe')
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$ProbeDirectory){$ProbeDirectory=$RuntimeDirectory}
$ProbeDirectory=[IO.Path]::GetFullPath($ProbeDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory;& "$PSScriptRoot\build-media-metadata-probe.ps1" -OutputDirectory $ProbeDirectory}
if(!(Test-Path -LiteralPath $SevenZip)){throw '7-Zip is required for the independent ISO metadata comparison'}
$session=Join-Path $PSScriptRoot ('test-output\media-08\metadata-'+[Guid]::NewGuid().ToString('N'))
$fixtures=Join-Path $session 'fixtures';New-Item -ItemType Directory -Path $session -Force|Out-Null
python "$PSScriptRoot\make-media-metadata-fixtures.py" $fixtures
if($LASTEXITCODE){throw 'Metadata fixture generation failed'}
$manifest=Get-Content -Raw -LiteralPath (Join-Path $fixtures 'metadata.json')|ConvertFrom-Json
$hashes=@{};foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$hashes[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
$hostDrives=@([IO.DriveInfo]::GetDrives()|ForEach-Object Name)
$prefix=Join-Path $session 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
function Invoke-Metadata([string[]]$arguments,[string]$name,[string]$loader=$runtime,[switch]$ExpectFailure){
    $saved=$ErrorActionPreference;$ErrorActionPreference='Continue'
    try{& $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $session "$name.log");$code=$LASTEXITCODE}
    finally{$ErrorActionPreference=$saved}
    if($ExpectFailure){if(!$code){throw "Malformed timestamp accepted: $name"}}
    elseif($code){throw "Metadata fixture failed: $name (exit $code)"}
}
$references=0
foreach($kind in @('primary','joliet')){
    $reference=Join-Path $session "reference-$kind"
    & $SevenZip x -y "-o$reference" (Join-Path $fixtures "$kind.iso") *> (Join-Path $session "7zip-$kind.log")
    if($LASTEXITCODE){throw 'Independent ISO extraction failed'}
    foreach($entry in $manifest.$kind.entries){
        if($entry.name -eq '.' -or $entry.unspecified){continue}
        $actual=(Get-Item -Force -LiteralPath (Join-Path $reference $entry.name)).LastWriteTimeUtc.ToFileTimeUtc()
        if($actual -ne [long]$entry.ticks){throw "Independent ISO timestamp disagrees: $kind/$($entry.name)"};$references++
    }
    foreach($bits in @(32,64)){
        $native=Join-Path $session "native-$kind-$bits";New-Item -ItemType Directory -Path $native|Out-Null
        & "$ProbeDirectory\media-metadata-probe$bits.exe" --native $native (Join-Path $fixtures "$kind.tsv") 2>&1|Tee-Object -FilePath (Join-Path $session "native-$kind-$bits.log")
        if($LASTEXITCODE){throw "Native metadata baseline failed: $kind-$bits"}
    }
}
Invoke-Metadata @('init') 'init'
$tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $tests|Out-Null
Copy-Item -LiteralPath "$ProbeDirectory\media-metadata-probe32.exe","$ProbeDirectory\media-metadata-probe64.exe" -Destination $tests
Copy-Item -LiteralPath (Join-Path $fixtures 'primary.tsv'),(Join-Path $fixtures 'joliet.tsv') -Destination $tests
foreach($kind in @('primary','joliet','cue')){
    $source=if($kind -eq 'cue'){Join-Path $fixtures 'disc.cue'}else{Join-Path $fixtures "$kind.iso"}
    $expected=if($kind -eq 'joliet'){'joliet'}else{'primary'}
    Invoke-Metadata @('drive','mount','D',$source) "mount-$kind"
    foreach($bits in @(32,64)){
        Invoke-Metadata @('run',"C:\tests\media-metadata-probe$bits.exe",'--mounted','D:\',"C:\tests\$expected.tsv") "$kind-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    }
}
$ini=Join-Path $prefix 'dosdevices\drives.ini';$iniHash=(Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash
$cache=Join-Path $prefix '.winedows\media';$count=@(Get-ChildItem -LiteralPath $cache -Directory).Count;$malformed=0
foreach($bad in Get-ChildItem -LiteralPath $fixtures -Filter 'bad-*.iso'){
    $loader=Join-Path $RuntimeDirectory $(if($malformed++%2){'wine32.exe'}else{'wine64.exe'})
    Invoke-Metadata @('drive','mount','D',$bad.FullName) $bad.BaseName -loader $loader -ExpectFailure
    if((Get-Content -Raw -LiteralPath (Join-Path $session ($bad.BaseName+'.log'))) -notmatch 'recording (date|time)'){throw 'Timestamp image failed for an unrelated reason'}
    if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $iniHash){throw 'Malformed timestamp changed mapping'}
}
if(@(Get-ChildItem -LiteralPath $cache -Directory).Count -ne $count){throw 'Malformed timestamp created a partial private cache'}
foreach($bits in @(32,64)){
    Invoke-Metadata @('run',"C:\tests\media-metadata-probe$bits.exe",'--mounted','D:\','C:\tests\primary.tsv','--swap',(Join-Path $fixtures 'joliet.iso'),'C:\tests\joliet.tsv',(Join-Path $RuntimeDirectory "wine$bits.exe")) "live-$bits" -loader (Join-Path $RuntimeDirectory "wine$bits.exe")
    if((Get-FileHash -LiteralPath $ini -Algorithm SHA256).Hash -ne $iniHash){throw 'Live metadata swap changed persistent mapping'}
}
foreach($path in $hashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$path]){throw "Original metadata fixture changed: $path"}}
if(Compare-Object $hostDrives @([IO.DriveInfo]::GetDrives()|ForEach-Object Name)){throw 'Host drive list changed'}
@{passed=$true;architectures=@('x86','x64');formats=@('ISO9660','Joliet','BIN/CUE');independentReferenceDates=$references;
    nativeCalendarAndErrorBaselines=$true;recordedUtcTimes=$true;hiddenFlags=$true;fileDirectoryAndRootMetadata=$true;
    unspecifiedDatePolicy='1970-01-01 UTC fallback';malformedRollbackCases=$malformed;liveSwapMetadata=$true;
    originalsAndPersistentMappingUnchanged=$true;hostDriveListUnchanged=$true;utc=[DateTime]::UtcNow.ToString('o')
}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $session 'result.json')
Write-Host "Media metadata suite passed: $session";$global:LASTEXITCODE=0
