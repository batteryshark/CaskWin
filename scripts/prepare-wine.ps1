<#
.SYNOPSIS
Prepare a separate, pinned Wine source tree for native CaskWin builds.
.DESCRIPTION
The default profile applies the reviewed Wine 11.0 patch series to a local clone
of vendor/wine. The vendor checkout is never patched or switched. Existing output
is reused only when its recorded profile, index and working tree still match.

For another Wine fork/version, provide a local clone with -SourceDirectory and
an explicit -Profile containing schemaVersion=1, name, upstreamUrl, commit (full
40-character commit ID), and ordered patches [{path,sha256}]. Patch paths are
relative to the profile directory. An empty series is valid for an already
adapted fork. -Ref may name that exact profile commit; it cannot bypass the pin.
Preparing sources does not establish build or runtime compatibility for a new
version. No network fetches are performed.
.EXAMPLE
.\scripts\prepare-wine.ps1
.EXAMPLE
.\scripts\prepare-wine.ps1 -Profile .\profiles\my-fork.json -SourceDirectory D:\src\my-wine -Ref native-port -OutputDirectory .\build\my-wine-source
#>
[CmdletBinding()]
param(
    [string]$Profile=(Join-Path (Split-Path $PSScriptRoot) 'wine-source.json'),
    [string]$SourceDirectory=(Join-Path (Split-Path $PSScriptRoot) 'vendor\wine'),
    [string]$OutputDirectory=(Join-Path (Split-Path $PSScriptRoot) 'build\wine-source'),
    [string]$Ref
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Invoke-SourceGit {
    param([string[]]$Arguments)
    $savedPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $output=@(& git @Arguments 2>&1)
        $code=$LASTEXITCODE
    } finally {$ErrorActionPreference=$savedPreference}
    if($code -ne 0){throw "Git failed (exit $code): git $($Arguments -join ' ')`n$($output -join "`n")"}
    return (($output | ForEach-Object {"$_"}) -join "`n").Trim()
}
function Read-SourceJson {
    param([string]$Path)
    try {return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json}
    catch {throw "Cannot read source JSON '$Path': $($_.Exception.Message)"}
}
function Assert-SourceField {
    param($Object,[string]$Name)
    if(!$Object -or !$Object.PSObject.Properties[$Name]){throw "Source metadata is missing '$Name'."}
}

$Profile=[IO.Path]::GetFullPath($Profile)
$SourceDirectory=[IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
if(!(Test-Path -LiteralPath $Profile -PathType Leaf)){throw "Wine source profile not found: $Profile"}
if(!(Test-Path -LiteralPath $SourceDirectory -PathType Container)){
    throw "Wine source checkout not found: $SourceDirectory. Initialize vendor/wine with git submodule update --init, or pass -SourceDirectory for a local checkout."
}
if($OutputDirectory.Equals($SourceDirectory,[StringComparison]::OrdinalIgnoreCase) -or
   $OutputDirectory.StartsWith($SourceDirectory+'\',[StringComparison]::OrdinalIgnoreCase)){
    throw 'OutputDirectory must be separate from SourceDirectory; the source/vendor checkout is never modified.'
}
$definition=Read-SourceJson $Profile
foreach($name in @('schemaVersion','name','upstreamUrl','commit','patches')){Assert-SourceField $definition $name}
if($definition.schemaVersion -ne 1){throw 'Unsupported Wine source profile schemaVersion; expected 1.'}
if($definition.commit -notmatch '^[0-9a-fA-F]{40}$'){throw 'Wine source profile commit must be a full 40-character Git commit ID.'}
if([string]::IsNullOrWhiteSpace($definition.name) -or [string]::IsNullOrWhiteSpace($definition.upstreamUrl)){throw 'Wine source profile name and upstreamUrl must be nonempty.'}
if($null -eq $definition.patches -or $definition.patches -isnot [Array]){throw 'Wine source profile patches must be a JSON array (possibly empty).'}
$profileDirectory=Split-Path $Profile
$profileHash=(Get-FileHash -LiteralPath $Profile -Algorithm SHA256).Hash.ToLowerInvariant()
$patchFiles=@()
$patchRecords=@()
foreach($patch in $definition.patches){
    foreach($name in @('path','sha256')){Assert-SourceField $patch $name}
    if([string]::IsNullOrWhiteSpace($patch.path) -or [IO.Path]::IsPathRooted($patch.path)){throw 'Patch paths must be relative to the profile directory.'}
    $path=[IO.Path]::GetFullPath((Join-Path $profileDirectory $patch.path))
    if(!$path.StartsWith($profileDirectory+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Patch path leaves the profile directory: $($patch.path)"}
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Wine patch not found: $path"}
    if($patch.sha256 -notmatch '^[0-9a-fA-F]{64}$'){throw "Invalid SHA256 for Wine patch: $($patch.path)"}
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if($hash -ne $patch.sha256.ToLowerInvariant()){throw "Wine patch checksum mismatch: $($patch.path)"}
    $patchFiles+=$path
    $patchRecords+=@{path=$patch.path;sha256=$hash}
}
$baseCommit=Invoke-SourceGit -Arguments @('-C',$SourceDirectory,'rev-parse','--verify',($definition.commit+'^{commit}'))
if($baseCommit -ne $definition.commit.ToLowerInvariant()){throw 'Resolved source commit does not match the profile pin.'}
if($Ref){
    $selectedCommit=Invoke-SourceGit -Arguments @('-C',$SourceDirectory,'rev-parse','--verify',($Ref+'^{commit}'))
    if($selectedCommit -ne $baseCommit){throw 'Ref does not match the profile commit. Select an explicit profile and matching patch series for that fork/version.'}
}
$markerPath=Join-Path $OutputDirectory '.caskwin-source.json'
if(Test-Path -LiteralPath $OutputDirectory){
    $occupied="Prepared output already exists and does not match this profile: $OutputDirectory. Preserve any changes and choose another -OutputDirectory, or remove it yourself after review."
    if(!(Test-Path -LiteralPath $markerPath -PathType Leaf)){throw $occupied}
    try {
        $marker=Read-SourceJson $markerPath
        foreach($name in @('schemaVersion','profileSha256','baseCommit','patchedTree')){Assert-SourceField $marker $name}
        if($marker.schemaVersion -ne 1 -or $marker.profileSha256 -ne $profileHash -or $marker.baseCommit -ne $baseCommit){throw $occupied}
        if((Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'rev-parse','HEAD')) -ne $baseCommit){throw $occupied}
        if((Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'write-tree')) -ne $marker.patchedTree){throw $occupied}
        $null=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'diff','--quiet','--no-ext-diff')
        $extras=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'ls-files','--others')
        if(@($extras -split "`n" | Where-Object {$_ -and $_ -ne '.caskwin-source.json'}).Count){throw $occupied}
    } catch {throw "$occupied`n$($_.Exception.Message)"}
    Write-Host "Verified prepared Wine source: $OutputDirectory"
    return
}

$parent=Split-Path $OutputDirectory
$null=New-Item -ItemType Directory -Force -Path $parent
try {
    # Local clone, with independent object storage; no vendor mutation or fetch.
    $null=Invoke-SourceGit -Arguments @('clone','--no-checkout','--no-hardlinks','--',$SourceDirectory,$OutputDirectory)
    $null=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'-c','core.autocrlf=false','checkout','--detach',$baseCommit)
    $null=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'config','core.autocrlf','false')
    foreach($path in $patchFiles){
        $null=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'apply','--check','--index','--',$path)
        $null=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'apply','--index','--',$path)
    }
    $tree=Invoke-SourceGit -Arguments @('-C',$OutputDirectory,'write-tree')
    $marker=[ordered]@{schemaVersion=1;profileSha256=$profileHash;baseCommit=$baseCommit;patchedTree=$tree;upstreamUrl=$definition.upstreamUrl;patches=@($patchRecords)}
    $marker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $markerPath -Encoding UTF8
} catch {
    throw "Wine source preparation failed. Any partial output was left at '$OutputDirectory' for inspection; choose a new output path before retrying. $($_.Exception.Message)"
}
Write-Host "Prepared Wine source: $OutputDirectory"
Write-Host "Profile: $($definition.name); base: $baseCommit; patched tree: $tree"
