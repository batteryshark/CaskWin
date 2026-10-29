param(
    [Parameter(Mandatory=$true)][string]$Prefix,
    [Parameter(Mandatory=$true)][string]$TrackDirectory,
    [ValidatePattern('^[A-Za-z]$')][string]$Drive='D',
    [ValidateSet('none','synthetic','iso')][string]$DataSource='none',
    [ValidateRange(0,1151849)][int]$DataSectors=0,
    [string]$FFmpeg='ffmpeg',
    [string]$Loader=''
)
$ErrorActionPreference='Stop'
if(!$Loader){$Loader=Join-Path $PSScriptRoot 'bin\wine64.exe';if(!(Test-Path -LiteralPath $Loader)){$Loader=Join-Path $PSScriptRoot 'wine64.exe'}}
$prefixPath=[IO.Path]::GetFullPath($Prefix)
$sourcePath=[IO.Path]::GetFullPath($TrackDirectory)
$decoder=(Get-Command $FFmpeg -ErrorAction Stop).Source
$tracks=@(Get-ChildItem -LiteralPath $sourcePath -File|Where-Object {$_.Name -match '^Track(\d{2})\.(ogg|wav|flac|mp3)$'}|Sort-Object Name)
if(!$tracks.Count){throw 'Expected numbered TrackNN.ogg/.wav/.flac/.mp3 files'}
$numbers=@{}
foreach($track in $tracks){$null=$track.BaseName -match '^Track(\d{2})$';$n=[int]$Matches[1];if($n -lt 1 -or $numbers.ContainsKey($n)){throw 'Track numbers must be unique and between 01 and 99'};$numbers[$n]=$track}
$ordered=@($numbers.Keys|Sort-Object)
$first=$ordered[0];$last=$ordered[-1]
if($DataSource -eq 'none'){
    if($first -ne 1 -or $DataSectors){throw 'Audio-only media requires Track01 and no data sectors. For Track02 onward, explicitly select -DataSource synthetic -DataSectors N or -DataSource iso -DataSectors N.'}
} elseif($first -ne 2 -or $DataSectors -le 0){throw 'A declared first data track requires Track02 onward and a positive exact/synthetic data sector count'}
for($n=$first;$n -le $last;$n++){if(!$numbers.ContainsKey($n)){throw "Missing Track$('{0:D2}' -f $n)"}}
& $Loader --prefix $prefixPath init
if($LASTEXITCODE){throw 'Could not initialize/lock prefix'}
$mediaRoot=Join-Path $prefixPath '.winedows\media'
foreach($path in @($prefixPath,(Join-Path $prefixPath '.winedows'),$mediaRoot)) {
    if(Test-Path -LiteralPath $path){if((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'CDDA storage cannot use reparse points'}}
    else {New-Item -ItemType Directory -Path $path|Out-Null}
}
$cache=Join-Path $mediaRoot ('cdda-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $cache|Out-Null
$manifest=@('[disc]','version=1',"tracks=$last","data_source=$DataSource")
if($DataSource -ne 'none'){$manifest+=@('[track01]','type=data',"sectors=$DataSectors")}
$sources=@();$totalSectors=[long]$DataSectors
foreach($n in $ordered) {
    $inputFile=$numbers[$n]
    $before=(Get-FileHash -LiteralPath $inputFile.FullName -Algorithm SHA256).Hash
    $name='track{0:D2}.pcm' -f $n;$output=Join-Path $cache $name
    # Decoding is an explicit preparation step. Games only read private PCM.
    & $decoder -hide_banner -loglevel error -nostdin -n -i $inputFile.FullName -map 0:a:0 -vn -sn -dn -ac 2 -ar 44100 -c:a pcm_s16le -f s16le $output
    if($LASTEXITCODE){throw "Could not decode $($inputFile.Name); mapping was not changed. Partial private cache: $cache"}
    $size=(Get-Item -LiteralPath $output).Length
    if(!$size -or $size%4){throw "Invalid decoded PCM length for $($inputFile.Name)"}
    $sectors=[long][Math]::Ceiling($size/2352.0);$padded=$sectors*2352
    if($padded -ne $size){$stream=[IO.File]::Open($output,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None);try{$stream.SetLength($padded);$stream.Flush($true)}finally{$stream.Dispose()}}
    $totalSectors+=$sectors
    if($totalSectors+150 -ge 256*60*75){throw 'Track collection exceeds representable CD MSF addresses'}
    if((Get-FileHash -LiteralPath $inputFile.FullName -Algorithm SHA256).Hash -ne $before){throw 'Source changed during preparation; mapping was not changed'}
    [IO.File]::SetAttributes($output,[IO.FileAttributes]::ReadOnly)
    $manifest+=@(('[track{0:D2}]' -f $n),'type=audio',"file=$name","sectors=$sectors")
    $sources+=@{track=$n;source=$inputFile.FullName;sha256=$before;pcmSha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash;decodedBytes=$size;sectors=$sectors}
    Write-Host "Prepared track $n ($sectors sectors)"
}
$manifestPath=Join-Path $cache 'disc.ini'
[IO.File]::WriteAllLines($manifestPath,$manifest,[Text.Encoding]::Unicode)
[IO.File]::SetAttributes($manifestPath,[IO.FileAttributes]::ReadOnly)
@{dataSource=$DataSource;dataSectors=$DataSectors;tracks=$sources;decoder=$decoder;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $cache 'preparation.json')
& $Loader --prefix $prefixPath drive audio $Drive $manifestPath
if($LASTEXITCODE){throw "Prepared medium was not mounted. Private cache retained: $cache"}
if($DataSource -eq 'synthetic'){Write-Host "Using explicitly synthetic data track 1: $DataSectors sectors. These are not measured original-disc offsets."}
Write-Host "CDDA medium ready: $manifestPath"
