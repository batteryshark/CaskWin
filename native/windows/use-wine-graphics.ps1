param(
    [Parameter(Mandatory)][string]$Prefix,
    [Parameter(Mandatory)][string]$Program,
    [switch]$Restore
)
$ErrorActionPreference='Stop'
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
$prefixPath=[IO.Path]::GetFullPath($Prefix).TrimEnd('\')
$mapped=& $runtime --prefix $prefixPath path $Program
if($LASTEXITCODE){throw 'Cannot resolve the prefixed program.'}
$exe=[IO.Path]::GetFullPath($mapped)
if(!$exe.StartsWith($prefixPath+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'The executable must be a private copy inside the selected prefix.'}
if(!(Test-Path -LiteralPath $exe -PathType Leaf)){throw "Program not found: $exe"}
$directory=Split-Path $exe
$lockPath=Join-Path $prefixPath '.winedows\session.lock'
$lock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    $manifest=Join-Path $directory '.winedows-wine-graphics.json'
    if($Restore){
        if(!(Test-Path -LiteralPath $manifest)){throw 'No Wine graphics installation record exists in this application directory.'}
        $record=Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json
        foreach($item in $record.files){
            if($item.name -notin @('ddraw.dll','d3d8.dll','d3d9.dll','wined3d.dll')){throw 'Invalid installation record.'}
            $target=Join-Path $directory $item.name
            if(!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ne $item.hash){throw "DLL changed since installation; preserve it before restoring: $target"}
            if($item.backup -and !(Test-Path -LiteralPath ($target+'.winedows-original'))){throw "Missing original DLL backup: $target"}
        }
        foreach($item in $record.files){
            $target=Join-Path $directory $item.name
            if($item.backup){Move-Item -LiteralPath ($target+'.winedows-original') -Destination $target -Force}
            else{Remove-Item -LiteralPath $target}
        }
        Remove-Item -LiteralPath $manifest
        Write-Host "Restored the previous graphics DLLs in $directory"
        return
    }
    if(Test-Path -LiteralPath $manifest){throw 'Wine graphics is already installed here. Restore it before switching builds.'}
    $stream=[IO.File]::OpenRead($exe)
    try {
        $reader=[IO.BinaryReader]::new($stream)
        if($reader.ReadUInt16() -ne 0x5a4d){throw 'Program is not a Windows PE executable.'}
        $stream.Position=0x3c
        $pe=$reader.ReadUInt32()
        $stream.Position=$pe
        if($reader.ReadUInt32() -ne 0x4550){throw 'Invalid PE signature.'}
        $machine=$reader.ReadUInt16()
        $arch=switch($machine){0x14c{'x86'} 0x8664{'x64'} default{throw 'Only x86 and x64 programs are supported.'}}
    } finally {$stream.Dispose()}
    $source=Join-Path $PSScriptRoot "bin\builtin\$arch"
    $items=@()
    foreach($name in @('ddraw.dll','d3d8.dll','d3d9.dll','wined3d.dll')){
        $from=Join-Path $source $name
        if(!(Test-Path -LiteralPath $from)){throw "Build the Wine DLLs first: build-wine.ps1 -Arch $arch"}
        $target=Join-Path $directory $name
        if(Test-Path -LiteralPath ($target+'.winedows-original')){throw "Original backup already exists: $target"}
        $items+=@{name=$name;hash=(Get-FileHash -LiteralPath $from).Hash;backup=(Test-Path -LiteralPath $target)}
    }
    $done=@()
    try {
        foreach($item in $items){
            $target=Join-Path $directory $item.name
            if($item.backup){Move-Item -LiteralPath $target -Destination ($target+'.winedows-original')}
            $done+=$item
            Copy-Item -LiteralPath (Join-Path $source $item.name) -Destination $target
        }
        @{wine='11.0';arch=$arch;files=$items} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest
    } catch {
        foreach($item in $done){
            $target=Join-Path $directory $item.name
            if($item.backup){Move-Item -LiteralPath ($target+'.winedows-original') -Destination $target -Force}
            elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target}
        }
        throw
    }
    Write-Host "Installed Wine 11.0 DirectDraw/Direct3D 8/9 ($arch) in $directory"
} finally {$lock.Dispose()}
