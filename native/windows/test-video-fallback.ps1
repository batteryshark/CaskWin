param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime-04'))
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\video-native-fallback-'+[Guid]::NewGuid().ToString('N'))
$stage=Join-Path $suite 'runtime';$prefix=Join-Path $suite 'prefix';New-Item -ItemType Directory -Force -Path $stage|Out-Null
foreach($file in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll','video-probe32.exe','video-probe64.exe')){Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $file) -Destination $stage}
if(Test-Path -LiteralPath (Join-Path $stage 'builtin')){throw 'Fallback test must have no bundled Wine modules'}
$runtime=Join-Path $stage 'wine64.exe';$saved=$env:WINEDLLOVERRIDES;$results=@()
try{
    $env:WINEDLLOVERRIDES='dxgi,d3d11=n;mfreadwrite=b,n'
    & $runtime --prefix $prefix init *> (Join-Path $suite 'init.log');if($LASTEXITCODE){throw 'Fallback prefix initialization failed'}
    $tests=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Force -Path $tests|Out-Null
    & (Join-Path $stage 'video-probe64.exe') --generate (Join-Path $tests 'generated.mp4') *> (Join-Path $suite 'generate.log');if($LASTEXITCODE){throw 'Fallback sample generation failed'}
    foreach($bits in @(32,64)){
        Copy-Item -LiteralPath (Join-Path $stage "video-probe$bits.exe") -Destination $tests
        $log=Join-Path $suite "fallback-$bits.log"
        & $runtime --prefix $prefix --desktop off run "C:\tests\video-probe$bits.exe" --decode 'C:\tests\generated.mp4' *> $log
        $code=$LASTEXITCODE;$text=Get-Content -Raw -LiteralPath $log
        if($code -or $text -notmatch 'SOURCE READER OBJECT .*\\Windows\\(System32|SysWOW64)\\mfreadwrite.dll' -or $text -notmatch 'HARDWARE DECODE frames=24\b' -or $text -notmatch 'successful native DXVA decode submissions prove hardware use') {throw "Native video fallback failed: $bits ($code)"}
        $results+=@{architecture=$bits;status='passed';selected='native';wineModuleAbsent=$true};Write-Host "PASS native video fallback $bits"
    }
    @{passed=$true;cases=$results;overrides='dxgi,d3d11=n;mfreadwrite=b,n'}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Video fallback suite passed: $suite"
}finally{$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
