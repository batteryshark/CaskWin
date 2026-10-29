# Read-only validation of the portable Wine provenance shipped with a runtime.
. "$PSScriptRoot\wine-source.ps1"

function Get-CaskWinPortableWineIdentity {
    param([Parameter(Mandatory)]$Marker)
    if($Marker.schemaVersion -ne 1 -or
       $Marker.baseCommit -notmatch '^[0-9a-fA-F]{40}$' -or
       $Marker.patchedTree -notmatch '^[0-9a-fA-F]{40}$' -or
       $Marker.profileSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
       $Marker.upstreamUrl -isnot [string] -or
       $Marker.patches -isnot [array]){
        throw 'Invalid Wine provenance in build-source manifest.'
    }
    $patches=@(foreach($patch in $Marker.patches){
        if($patch.path -isnot [string] -or [string]::IsNullOrWhiteSpace($patch.path) -or
           $patch.sha256 -notmatch '^[0-9a-fA-F]{64}$'){
            throw 'Invalid ordered patch provenance in build-source manifest.'
        }
        [ordered]@{path=$patch.path;sha256=$patch.sha256.ToLowerInvariant()}
    })
    [ordered]@{
        schemaVersion=1
        baseCommit=$Marker.baseCommit.ToLowerInvariant()
        patchedTree=$Marker.patchedTree.ToLowerInvariant()
        profileSha256=$Marker.profileSha256.ToLowerInvariant()
        upstreamUrl=$Marker.upstreamUrl
        patches=$patches
    }|ConvertTo-Json -Depth 10 -Compress
}

function Assert-CaskWinPackageSource {
    param([Parameter(Mandatory)][string]$RuntimeDirectory,[string]$WineSourceDirectory)
    $source=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
    # Verify HEAD, index and working files before trusting the marker. The
    # returned local cache identity is not part of portable package provenance.
    $null=Get-CaskWinWineSourceIdentity -WineSourceDirectory $source
    $manifest=Join-Path $RuntimeDirectory 'caskwin-build-source.json'
    if(!(Test-Path -LiteralPath $manifest -PathType Leaf)){
        throw 'Missing runtime caskwin-build-source.json; build this runtime from the selected Wine source before packaging.'
    }
    $record=Get-Content -LiteralPath $manifest -Raw|ConvertFrom-Json
    if($record.schemaVersion -ne 1 -or $record.preparedMarkerSha256 -notmatch '^[0-9a-fA-F]{64}$' -or !$record.wine){
        throw 'Invalid runtime caskwin-build-source.json schema or prepared marker hash.'
    }
    $markerPath=Join-Path $source '.caskwin-source.json'
    $expectedHash=(Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash
    if($record.preparedMarkerSha256 -ne $expectedHash){
        throw 'Runtime Wine source marker differs from the selected prepared source; use the source used to build this runtime.'
    }
    $expected=Get-Content -LiteralPath $markerPath -Raw|ConvertFrom-Json
    if((Get-CaskWinPortableWineIdentity -Marker $record.wine) -cne
       (Get-CaskWinPortableWineIdentity -Marker $expected)){
        throw 'Runtime Wine commit, patched tree, profile or ordered patches differ from the selected prepared source.'
    }
    return $manifest
}
