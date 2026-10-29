param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
if(!$SkipBuild){& "$PSScriptRoot\build.ps1";& "$PSScriptRoot\build-wine.ps1";& "$PSScriptRoot\build-input-probe.ps1"}
$prefix=Join-Path $PSScriptRoot ('test-output\input-'+[Guid]::NewGuid().ToString('N'))
$runtime=Join-Path $PSScriptRoot 'bin\wine64.exe'
& $runtime --prefix $prefix init
if($LASTEXITCODE){throw 'Input prefix initialization failed'}
$directory=Join-Path $prefix 'drive_c\tests'
New-Item -ItemType Directory -Path $directory|Out-Null
foreach($bits in @(32,64)){
    Copy-Item -LiteralPath "$PSScriptRoot\bin\input-probe$bits.exe" -Destination (Join-Path $directory "dynamic$bits.exe")
    Copy-Item -LiteralPath "$PSScriptRoot\bin\input-import-probe$bits.exe" -Destination (Join-Path $directory "imports$bits.exe")
}
function Invoke-InputFixture([string]$name,[string[]]$arguments,[string]$loader=$runtime) {
    & $loader --prefix $prefix @arguments 2>&1|Tee-Object -FilePath (Join-Path $prefix "$name.log")
    if($LASTEXITCODE){throw "Input fixture failed: $name (exit $LASTEXITCODE)"}
}
$saved=$env:WINEDLLOVERRIDES
try {
    foreach($order in @('b,n','n,b')){
        $env:WINEDLLOVERRIDES="dinput,dinput8=$order"
        foreach($bits in @(32,64)){
            $other=if($bits -eq 32){64}else{32}
            $arguments=@('run',"C:\tests\imports$bits.exe",'--child-program',"C:\tests\dynamic$other.exe")
            if($order -eq 'n,b'){$arguments+='--native'}
            Invoke-InputFixture "order-$($order.Replace(',',''))-$bits" $arguments
        }
    }
    foreach($bits in @(32,64)){
        $env:WINEDLLOVERRIDES='dinput=b;dinput8=n'
        Invoke-InputFixture "mixed-old-wine-$bits" @('run',"C:\tests\dynamic$bits.exe",'--native-dinput8')
        $env:WINEDLLOVERRIDES='dinput=n;dinput8=b'
        Invoke-InputFixture "mixed-new-wine-$bits" @('run',"C:\tests\dynamic$bits.exe",'--native-dinput')
        foreach($mode in @('b','n')){
            $env:WINEDLLOVERRIDES="dinput,dinput8=$mode"
            $arguments=@('--desktop','Input,640x480','run',"C:\tests\imports$bits.exe")
            if($mode -eq 'n'){$arguments+='--native'}
            Invoke-InputFixture "desktop-$mode-$bits" $arguments
        }
    }
    $minimal=Join-Path $prefix '.winedows\runtime-without-builtins'
    New-Item -ItemType Directory -Path $minimal|Out-Null
    foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){
        Copy-Item -LiteralPath (Join-Path "$PSScriptRoot\bin" $name) -Destination $minimal
    }
    $minimalLoader=Join-Path $minimal 'wine64.exe'
    $env:WINEDLLOVERRIDES='dinput,dinput8=b,n'
    Invoke-InputFixture 'missing-builtin-native-fallback' @('run','C:\tests\imports64.exe','--native') $minimalLoader
    $env:WINEDLLOVERRIDES='dinput,dinput8=b'
    & $minimalLoader --prefix $prefix run 'C:\tests\imports64.exe' 2>&1|Tee-Object -FilePath (Join-Path $prefix 'strict-missing-builtin.log')
    if(!$LASTEXITCODE){throw 'Strict missing input builtins silently fell back'}
    if((Test-Path -LiteralPath (Join-Path $directory 'dinput.dll')) -or (Test-Path -LiteralPath (Join-Path $directory 'dinput8.dll'))){throw 'Input providers must not be copied beside the application'}
    @{
        passed=$true;architectures=@('x86','x64');providers=@('Wine','native');startupImports=$true;
        dynamicLoads=$true;crossBitnessChildren=$true;virtualDesktop=$true;missingBuiltinFallback=$true;
        untested=@('fresh physical key/mouse event delivery','physical controllers','force feedback');
        inputSynthesized=$false;inputContentsLogged=$false;utc=[DateTime]::UtcNow.ToString('o')
    }|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $prefix 'result.json')
    Write-Host "Input suite passed: $prefix"
}finally{$env:WINEDLLOVERRIDES=$saved}
$global:LASTEXITCODE=0
