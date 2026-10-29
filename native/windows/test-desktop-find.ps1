param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime09-driver'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\display-09\fixtures'),[switch]$SkipBuild,[switch]$NativeOnly)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-desktop-find-probe.ps1" -OutputDirectory $FixtureDirectory}
$output=Join-Path $PSScriptRoot ('test-output\display-09\find-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output|Out-Null
$ready=Join-Path $output 'host-handles.txt';$stop=Join-Path $output 'stop.txt'
$hostProcess=Start-Process -FilePath "$FixtureDirectory\desktop-find-probe64.exe" -ArgumentList @('--host',('"'+$ready+'"'),('"'+$stop+'"')) -WindowStyle Hidden -PassThru
$siblingProcess=$null;$siblingStop=$null
$results=[Collections.Generic.List[object]]::new();$saved=$env:WINEDLLOVERRIDES
function Invoke-Case([string]$Name,[string]$Program,[string[]]$Arguments){
    $log=Join-Path $output "$Name.log"
    & $Program @Arguments 2>&1|Tee-Object -FilePath $log|Out-Host
    $code=$LASTEXITCODE;$results.Add(@{name=$Name;exitCode=$code;passed=($code -eq 0);log=$log})
}
try {
    $env:WINEDLLOVERRIDES=$null
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path -LiteralPath $ready)-and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 20}
    if(!(Test-Path -LiteralPath $ready)){throw 'Owned native fixture host did not become ready'}
    $handles=((Get-Content -LiteralPath $ready -Raw).Trim() -split ' ')
    foreach($bits in @(32,64)){Invoke-Case "native-$bits" "$FixtureDirectory\desktop-find-probe$bits.exe" (@('--native')+$handles)}
    if(!$NativeOnly){
        $prefix=Join-Path $output 'prefix';$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
        & $runtime --prefix $prefix init
        if($LASTEXITCODE){throw 'Prefix initialization failed'}
        $directory=Join-Path $prefix 'drive_c\tests';New-Item -ItemType Directory -Path $directory|Out-Null
        foreach($bits in @(32,64)){Copy-Item -LiteralPath "$FixtureDirectory\desktop-find-probe$bits.exe" -Destination "$directory\find$bits.exe"}
        foreach($bits in @(32,64)){Invoke-Case "prefix-native-$bits" $runtime (@('--prefix',$prefix,'run',"C:\tests\find$bits.exe",'--native')+$handles)}
        # A second real prefix/container owns identically named hidden roots.
        # This proves the desktop-HWND tag excludes sibling-prefix helpers too.
        $sibling=Join-Path $output 'sibling-prefix'
        & $runtime --prefix $sibling init
        if($LASTEXITCODE){throw 'Sibling prefix initialization failed'}
        $siblingTests=Join-Path $sibling 'drive_c\tests';New-Item -ItemType Directory -Path $siblingTests|Out-Null
        Copy-Item -LiteralPath "$FixtureDirectory\desktop-find-probe64.exe" -Destination "$siblingTests\find64.exe"
        $siblingReady=Join-Path $siblingTests 'ready.txt';$siblingStop=Join-Path $siblingTests 'stop.txt'
        $siblingProcess=Start-Process -FilePath $runtime -ArgumentList @('--prefix',('"'+$sibling+'"'),'--desktop','Sibling,800x600','run','C:\tests\find64.exe','--host','C:\tests\ready.txt','C:\tests\stop.txt') -WindowStyle Hidden -PassThru
        $deadline=[DateTime]::UtcNow.AddSeconds(10)
        while(!(Test-Path -LiteralPath $siblingReady)-and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 20}
        if(!(Test-Path -LiteralPath $siblingReady)){throw 'Sibling fixture host did not become ready'}
        $siblingHandles=((Get-Content -LiteralPath $siblingReady -Raw).Trim() -split ' ')
        foreach($bits in @(32,64)){
            foreach($child in @(32,64)){
                Invoke-Case "virtual-$bits-to-$child" $runtime (@('--prefix',$prefix,'--desktop','Find,800x600','run',"C:\tests\find$bits.exe",'--virtual')+$siblingHandles+@("C:\tests\find$child.exe"))
            }
        }
    }
    $passed=!($results|Where-Object {!$_.passed})
    @{passed=$passed;runtime=$RuntimeDirectory;noGlobalInput=$true;cases=$results}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$output\result.json"
    Write-Host "Desktop find results: $output"
    if(!$passed){throw "Desktop find regression failed: $output"}
} finally {
    $env:WINEDLLOVERRIDES=$saved
    if($siblingProcess){
        Set-Content -LiteralPath $siblingStop -Value done
        if(!$siblingProcess.WaitForExit(5000)){$siblingProcess.Kill();$siblingProcess.WaitForExit()}
        $siblingProcess.Dispose()
    }
    Set-Content -LiteralPath $stop -Value done
    if(!$hostProcess.WaitForExit(5000)){$hostProcess.Kill();$hostProcess.WaitForExit()}
    $hostProcess.Dispose()
}
$global:LASTEXITCODE=0
