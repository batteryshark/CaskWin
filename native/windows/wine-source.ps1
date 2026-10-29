# Shared, read-only source resolution. Preparation is always an explicit step.
function Get-CaskWinWineSource {
    param([string]$WineSourceDirectory)
    if(!$WineSourceDirectory){$WineSourceDirectory=$env:CASKWIN_WINE_SOURCE}
    if(!$WineSourceDirectory){$WineSourceDirectory=Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'build\wine-source'}
    $source=[IO.Path]::GetFullPath($WineSourceDirectory)
    foreach($relative in @('configure','include','server\unicode.c','.caskwin-source.json')){
        if(!(Test-Path -LiteralPath (Join-Path $source $relative))){
            throw "Prepared Wine source is missing $relative at $source. Run scripts/prepare-wine.ps1 for the selected source profile first, or pass -WineSourceDirectory."
        }
    }
    $marker=Get-Content -LiteralPath (Join-Path $source '.caskwin-source.json') -Raw|ConvertFrom-Json
    if($marker.schemaVersion -ne 1 -or $marker.baseCommit -notmatch '^[0-9a-fA-F]{40}$' -or $marker.patchedTree -notmatch '^[0-9a-fA-F]{40}$' -or $marker.profileSha256 -notmatch '^[0-9a-fA-F]{64}$'){
        throw "Unsupported prepared Wine source marker at $source; prepare the selected profile again."
    }
    return $source
}

function Get-CaskWinWineSourceIdentity {
    param([Parameter(Mandatory)][string]$WineSourceDirectory)
    $source=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
    $commit=(& git -C $source rev-parse HEAD 2>$null|Out-String).Trim()
    if($LASTEXITCODE -or $commit -notmatch '^[0-9a-fA-F]{40}$'){throw 'Cannot identify prepared Wine source commit'}
    $markerPath=Join-Path $source '.caskwin-source.json'
    $marker=Get-Content -LiteralPath $markerPath -Raw|ConvertFrom-Json
    if($commit -ne $marker.baseCommit){throw 'Prepared Wine HEAD differs from its source marker; run source preparation again'}
    & git -C $source diff --quiet
    if($LASTEXITCODE){throw 'Prepared Wine source has unstaged changes; source preparation must validate it before building'}
    $tree=(& git -C $source write-tree 2>$null|Out-String).Trim()
    if($LASTEXITCODE -or $tree -ne $marker.patchedTree){throw 'Prepared Wine index differs from its patched-tree marker; run source preparation again'}
    $extra=@(& git -C $source ls-files --others --exclude-standard --directory)
    if($LASTEXITCODE){throw 'Cannot inspect extra files in prepared Wine source'}
    $extra+=@(& git -C $source ls-files --others --ignored --exclude-standard --directory)
    if($LASTEXITCODE){throw 'Cannot inspect ignored files in prepared Wine source'}
    if(@($extra|Where-Object {$_ -and $_ -ne '.caskwin-source.json'}).Count){throw 'Prepared Wine source contains unrecorded files; run source preparation validation before building'}
    $markerHash=(Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash
    $inputBytes=[Text.Encoding]::UTF8.GetBytes($source.Replace('\','/').ToLowerInvariant()+"`n"+$commit+"`n"+$markerHash)
    $sha=[Security.Cryptography.SHA256]::Create()
    try {return ([BitConverter]::ToString($sha.ComputeHash($inputBytes))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
}

function Assert-CaskWinBuildOutput {
    param([Parameter(Mandatory)][string]$WineSourceDirectory,[Parameter(Mandatory)][string]$OutputDirectory)
    # Verify source content, then compare portable provenance rather than the
    # local source directory: equivalent prepared copies may share an output.
    $null=Get-CaskWinWineSourceIdentity -WineSourceDirectory $WineSourceDirectory
    $provenance=Join-Path $OutputDirectory 'caskwin-build-source.json'
    if(Test-Path -LiteralPath $provenance){
        $previous=Get-Content -LiteralPath $provenance -Raw|ConvertFrom-Json
        $expected=(Get-FileHash -LiteralPath (Join-Path $WineSourceDirectory '.caskwin-source.json') -Algorithm SHA256).Hash
        if($previous.schemaVersion -ne 1 -or $previous.preparedMarkerSha256 -ne $expected){
            throw "Output directory contains another Wine source/profile/patch build: $OutputDirectory. Choose a new -OutputDirectory."
        }
    } else {
        $artifacts=@('wine.exe','wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')|Where-Object {Test-Path -LiteralPath (Join-Path $OutputDirectory $_)}
        $builtins=Join-Path $OutputDirectory 'builtin'
        if(Test-Path -LiteralPath $builtins){$artifacts+=@(Get-ChildItem -LiteralPath $builtins -Filter '*.dll' -File -Recurse)}
        if(@($artifacts).Count){throw "Output directory contains runtime artifacts without source provenance: $OutputDirectory. Choose a new -OutputDirectory."}
    }
}

function Initialize-CaskWinBuildOutput {
    param([Parameter(Mandatory)][string]$WineSourceDirectory,[Parameter(Mandatory)][string]$OutputDirectory)
    Assert-CaskWinBuildOutput -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
    New-Item -ItemType Directory -Force -Path $OutputDirectory|Out-Null
    # Identity is established before any compiler output, including a failed
    # build. This records the source, not build/test completion or module count.
    Write-CaskWinBuildSource -WineSourceDirectory $WineSourceDirectory -OutputDirectory $OutputDirectory
}

function Assert-CaskWinWineBuild {
    param([Parameter(Mandatory)][string]$WineSourceDirectory,[Parameter(Mandatory)][ValidateSet('x86','x64')][string]$Arch)
    $build=Join-Path $PSScriptRoot "build\wine-$Arch"
    $stamp=Join-Path $build '.caskwin-source-id'
    $identity=Get-CaskWinWineSourceIdentity -WineSourceDirectory $WineSourceDirectory
    if(!(Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $identity){
        throw "Wine $Arch object tree does not match the selected source/profile/patches. Run build-wine.ps1 for this source; remove or archive an old native/windows/build/wine-$Arch directory before switching profiles."
    }
    return $build
}

function Write-CaskWinBuildSource {
    param([Parameter(Mandatory)][string]$WineSourceDirectory,[Parameter(Mandatory)][string]$OutputDirectory)
    $source=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
    Assert-CaskWinBuildOutput -WineSourceDirectory $source -OutputDirectory $OutputDirectory
    $markerPath=Join-Path $source '.caskwin-source.json'
    $marker=Get-Content -LiteralPath $markerPath -Raw|ConvertFrom-Json
    [ordered]@{
        schemaVersion=1
        wine=$marker
        preparedMarkerSha256=(Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash
    }|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'caskwin-build-source.json') -Encoding utf8
}
