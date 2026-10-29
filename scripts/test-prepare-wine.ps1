<#
.SYNOPSIS
Validate the default Wine source preparation and its refusal paths, without compiling.
.DESCRIPTION
Creates disposable evidence beneath build/prepare-validation-*. It never deletes
existing directories or changes vendor/wine. Optionally use -ReferenceDirectory
to compare all patched files byte-for-byte with the previously tested Wine tree.
#>
[CmdletBinding()]
param(
    [string]$SourceDirectory=(Join-Path (Split-Path $PSScriptRoot) 'vendor\wine'),
    [string]$PreparedDirectory=(Join-Path (Split-Path $PSScriptRoot) 'build\wine-source'),
    [string]$ReferenceDirectory
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$prepare=Join-Path $PSScriptRoot 'prepare-wine.ps1'
$profile=Join-Path $root 'wine-source.json'
$definition=Get-Content -Raw -LiteralPath $profile | ConvertFrom-Json
$suite=Join-Path $root ('build\prepare-validation-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $suite
$checks=[Collections.Generic.List[object]]::new()
function Git-Read {
    param([string[]]$Arguments)
    $output=@(& git @Arguments)
    if($LASTEXITCODE){throw "Git read failed: $($Arguments -join ' ')"}
    return (($output | ForEach-Object {"$_"}) -join "`n").Trim()
}
function Expect-PrepareFailure {
    param([string]$Name,[string]$ProfilePath,[string]$Destination,[string]$Expected)
    $caught=$null
    try {& $prepare -Profile $ProfilePath -SourceDirectory $SourceDirectory -OutputDirectory $Destination}
    catch {$caught=$_.Exception.Message}
    if(!$caught -or $caught -notmatch $Expected){throw "Expected $Name failure was not observed: $caught"}
    $checks.Add(@{name=$Name;passed=$true})
}
$vendorHead=Git-Read -Arguments @('-C',$SourceDirectory,'rev-parse','HEAD')
$vendorStatus=Git-Read -Arguments @('-C',$SourceDirectory,'status','--porcelain')
if($vendorHead -ne $definition.commit -or $vendorStatus){throw 'Validation requires the default pristine, pinned vendor checkout.'}
& $prepare -Profile $profile -SourceDirectory $SourceDirectory -OutputDirectory $PreparedDirectory -Ref HEAD
$marker=Join-Path $PreparedDirectory '.caskwin-source.json'
$markerHash=(Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash
& $prepare -Profile $profile -SourceDirectory $SourceDirectory -OutputDirectory $PreparedDirectory
if((Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash -ne $markerHash){throw 'Matching-source reuse changed provenance.'}
$checks.Add(@{name='prepare-and-reuse';passed=$true})
$changed=@((Git-Read -Arguments @('-C',$PreparedDirectory,'diff','--cached','--name-only')) -split "`n" | Where-Object {$_})
if($changed.Count -ne 45){throw "Expected the reviewed 45-path Wine patch, got $($changed.Count) paths."}
if($ReferenceDirectory){
    foreach($path in $changed){
        $expected=Join-Path $ReferenceDirectory $path
        $actual=Join-Path $PreparedDirectory $path
        if(!(Test-Path -LiteralPath $expected -PathType Leaf) -or
           (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $actual -Algorithm SHA256).Hash){
            throw "Prepared source differs from the tested reference: $path"
        }
    }
    $checks.Add(@{name='reference-file-equivalence';passed=$true;files=$changed.Count})
}

# Profiles and patches for refusal tests are confined to this new evidence folder.
$validPatch=Join-Path $suite 'native-windows.patch'
Copy-Item -LiteralPath (Join-Path $root $definition.patches[0].path) -Destination $validPatch
$badChecksum=Join-Path $suite 'bad-checksum.json'
@{schemaVersion=1;name='bad-checksum-control';upstreamUrl=$definition.upstreamUrl;commit=$definition.commit;patches=@(@{path='native-windows.patch';sha256=('0'*64)})} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $badChecksum -Encoding UTF8
$checksumOutput=Join-Path $suite 'checksum-output'
Expect-PrepareFailure 'checksum-rejected' $badChecksum $checksumOutput 'checksum mismatch'
if(Test-Path -LiteralPath $checksumOutput){throw 'Checksum rejection created an output tree.'}

$occupied=Join-Path $suite 'occupied'
$null=New-Item -ItemType Directory -Path $occupied
$sentinel=Join-Path $occupied 'preserve.txt'
'existing work must survive' | Set-Content -LiteralPath $sentinel -Encoding UTF8
$sentinelHash=(Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
Expect-PrepareFailure 'occupied-output-rejected' $profile $occupied 'already exists and does not match'
if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $sentinelHash -or @(Get-ChildItem -LiteralPath $occupied -Force).Count -ne 1){throw 'Occupied-output rejection changed existing work.'}

$badPatch=Join-Path $suite 'nonapplying.patch'
@'
diff --git a/LICENSE b/LICENSE
--- a/LICENSE
+++ b/LICENSE
@@ -1 +1 @@
-CASKWIN_VALIDATION_CONTEXT_THAT_IS_NOT_IN_WINE
+invalid replacement
'@ | Set-Content -LiteralPath $badPatch -Encoding ASCII
$badProfile=Join-Path $suite 'nonapplying.json'
@{schemaVersion=1;name='nonapplying-control';upstreamUrl=$definition.upstreamUrl;commit=$definition.commit;patches=@(@{path='nonapplying.patch';sha256=(Get-FileHash -LiteralPath $badPatch -Algorithm SHA256).Hash})} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $badProfile -Encoding UTF8
$failedOutput=Join-Path $suite 'nonapplying-output'
Expect-PrepareFailure 'nonapplying-patch-rejected' $badProfile $failedOutput 'patch failed|patch does not apply'
if(Test-Path -LiteralPath (Join-Path $failedOutput '.caskwin-source.json')){throw 'Failed patch application wrote a success marker.'}
if((Git-Read -Arguments @('-C',$failedOutput,'status','--porcelain'))){throw 'Failed patch check modified the cloned source.'}

# A copied marker cannot authorize reuse of a different index/tree.
Copy-Item -LiteralPath $marker -Destination (Join-Path $failedOutput '.caskwin-source.json')
Expect-PrepareFailure 'mismatched-prepared-tree-rejected' $profile $failedOutput 'already exists and does not match'
if((Git-Read -Arguments @('-C',$SourceDirectory,'rev-parse','HEAD')) -ne $vendorHead -or
   (Git-Read -Arguments @('-C',$SourceDirectory,'status','--porcelain')) -ne $vendorStatus){throw 'Vendor checkout changed during validation.'}
$checks.Add(@{name='vendor-remains-clean-and-pinned';passed=$true})
$result=@{passed=$true;checks=@($checks.ToArray());changedFiles=$changed;preparedDirectory=[IO.Path]::GetFullPath($PreparedDirectory);vendorCommit=$vendorHead}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $suite 'result.json') -Encoding UTF8
Write-Host "Wine source preparation validation passed: $suite"
