param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-release'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\fixtures'),[string]$GraphicsFixtureDirectory=(Join-Path $PSScriptRoot 'test-output\helpers-08\fixtures'),[string]$ImportFixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\import-fixtures'),[switch]$SkipBuild,[switch]$BindingOnly,[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\wine-source.ps1"
$wineSource=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$fontSource=Join-Path $wineSource 'dlls\gdi32\tests\wine_test.ttf'
if(!(Test-Path -LiteralPath $fontSource -PathType Leaf)){throw "Missing Wine font fixture: $fontSource"}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory);$GraphicsFixtureDirectory=[IO.Path]::GetFullPath($GraphicsFixtureDirectory)
$ImportFixtureDirectory=[IO.Path]::GetFullPath($ImportFixtureDirectory)
if(!$SkipBuild){& "$PSScriptRoot\build-builtin-startup-probe.ps1" -OutputDirectory $FixtureDirectory}
$suite=Join-Path $PSScriptRoot ('test-output\startup-builtins-08\binding-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix';New-Item -ItemType Directory -Path $suite|Out-Null
$font=Join-Path $suite 'wine_test.ttf';Copy-Item -LiteralPath $fontSource -Destination $font
$cases=[Collections.Generic.List[object]]::new();$tracked=@{}
function Track([string]$Path){$tracked[$Path]=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function Invoke-Case([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Overrides='',[string]$Expected='pass'){
    $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
    $start.Environment['WINEDLLOVERRIDES']=$Overrides
    $start.Environment['WD_STARTUP_SENTINEL']='prefix-env-preserved'
    $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(45000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$process.Dispose()
    [IO.File]::WriteAllText((Join-Path $suite "$Name.log"),$output)
    $pass=switch($Expected){'transitive-rejected'{$code -eq 1 -and $output -match 'Selected Wine startup provider conflicts with a transitive side-by-side import' -and $output -notmatch 'TRANSITIVE_PROVIDER'}'binding-rejected'{$code -eq 0 -and $output -match 'PASS expected binding rejection' -and $output -notmatch 'Provider='}default{$code -eq 0}}
    $cases.Add(@{name=$Name;passed=[bool]$pass;exitCode=$code;expected=$Expected});Write-Host "$Name exit=$code passed=$pass"
    if(!$pass){Write-Host $output;throw "Selected builtin startup case failed: $Name"}
}
try{
    foreach($bits in @(32,64)){
        $layout=Join-Path $suite "app$bits";New-Item -ItemType Directory -Path $layout|Out-Null
        Copy-Item -LiteralPath "$GraphicsFixtureDirectory\gdiplus-import-probe$bits.exe" -Destination "$layout\app.exe"
        Copy-Item -LiteralPath "$GraphicsFixtureDirectory\gdiplus-import-probe$bits.exe" -Destination "$layout\other.exe"
        Track "$GraphicsFixtureDirectory\gdiplus-import-probe$bits.exe"
        $arch=if($bits -eq 32){'x86'}else{'x64'};Track "$RuntimeDirectory\builtin\$arch\gdiplus.dll"
    }
    foreach($parent in @(32,64)){foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'};$machine=if($bits -eq 32){'14c'}else{'8664'}
        $arguments=@("$FixtureDirectory\startup-imports-helper$parent.dll","$suite\app$bits\app.exe",$machine,"$RuntimeDirectory\builtin\$arch\gdiplus.dll",$font,"$suite\binding-$parent-to-$bits.png","$suite\app$bits\other.exe",'bind')
        Invoke-Case "binding-$parent-to-$bits" "$FixtureDirectory\builtin-binding-probe$parent.exe" $arguments
        if($parent -eq $bits){foreach($mode in @('omit','descriptor-change','lookup-change','wrong-image')){
            $arguments[-1]=$mode;Invoke-Case "binding-$mode-$bits" "$FixtureDirectory\builtin-binding-probe$parent.exe" $arguments '' 'binding-rejected'
        }}
    }}
    foreach($bits in @(32,64)){
        $machine=if($bits -eq 32){'14c'}else{'8664'};$bad=Join-Path $suite "missing-export$bits";New-Item -ItemType Directory -Path $bad|Out-Null
        Copy-Item -LiteralPath "$ImportFixtureDirectory\native-startup-builtin$bits.dll" -Destination "$bad\gdiplus.dll"
        Invoke-Case "missing-selected-export-$bits" "$FixtureDirectory\builtin-binding-probe$bits.exe" @("$FixtureDirectory\startup-imports-helper$bits.dll","$suite\app$bits\app.exe",$machine,"$bad\gdiplus.dll",$font,"$suite\must-not-exist-$bits.png","$suite\app$bits\other.exe",'missing-export') '' 'binding-rejected'
    }
    if(!$BindingOnly){
        $runtime=Join-Path $RuntimeDirectory 'wine64.exe';& $runtime --prefix $prefix init
        if($LASTEXITCODE){throw 'Cannot initialize selected builtin prefix'}
        $native=@{}
        foreach($bits in @(32,64)){
            $native[$bits]=(& "$GraphicsFixtureDirectory\gdiplus-probe$bits.exe" --native-path).Trim();if($LASTEXITCODE){throw 'Cannot discover native GDI+ identity'}
            $app=Join-Path $prefix "drive_c\tests$bits";New-Item -ItemType Directory -Path $app|Out-Null
            Copy-Item -LiteralPath "$FixtureDirectory\builtin-child-probe$bits.exe" -Destination "$app\parent.exe"
            Copy-Item -LiteralPath "$FixtureDirectory\builtin-transitive-probe$bits.exe" -Destination "$app\transitive.exe"
            Copy-Item -LiteralPath "$FixtureDirectory\builtin-transitive$bits.dll" -Destination "$app\builtin-transitive.dll"
            Copy-Item -LiteralPath "$GraphicsFixtureDirectory\gdiplus-import-probe$bits.exe" -Destination "$app\static.exe"
            Copy-Item -LiteralPath $font -Destination "$app\font.ttf"
        }
        foreach($parent in @(32,64)){foreach($bits in @(32,64)){
            $arch=if($bits -eq 32){'x86'}else{'x64'}
            foreach($provider in @('wine','native')){
                $expected=if($provider -eq 'wine'){"$RuntimeDirectory\builtin\$arch\gdiplus.dll"}else{$native[$bits]}
                $childOrder=if($provider -eq 'wine'){'gdiplus=b'}else{'gdiplus=n'};$parentOrder=if($provider -eq 'wine'){'gdiplus=n'}else{'gdiplus=b'}
                Invoke-Case "child-$provider-$parent-to-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests$parent\parent.exe","C:\tests$bits\static.exe",$expected,"C:\tests$bits\font.ttf","C:\tests$bits\child-$provider-$parent.png",$childOrder) $parentOrder
            }
        }}
        foreach($bits in @(32,64)){
            Invoke-Case "transitive-native-$bits" $runtime @('--prefix',$prefix,'run',"C:\tests$bits\transitive.exe",$native[$bits]) 'gdiplus=n'
            foreach($order in @('b','b,n')){Invoke-Case "transitive-wine-rejected-$bits-$order" $runtime @('--prefix',$prefix,'run',"C:\tests$bits\transitive.exe",$native[$bits]) "gdiplus=$order" 'transitive-rejected'}
        }
        $sxsRuntime=Join-Path $suite 'sxs-runtime';New-Item -ItemType Directory -Path $sxsRuntime|Out-Null
        foreach($file in @('wine32.exe','wine64.exe','winedows32.dll','winedows64.dll')){Copy-Item -LiteralPath "$RuntimeDirectory\$file" -Destination $sxsRuntime}
        foreach($bits in @(32,64)){
            $arch=if($bits -eq 32){'x86'}else{'x64'};$processor=if($bits -eq 32){'x86'}else{'amd64'}
            $app=Join-Path $prefix "drive_c\sxs$bits";$assembly=Join-Path $app 'Winedows.Startup.SideBySide';$builtin=Join-Path $sxsRuntime "builtin\$arch"
            New-Item -ItemType Directory -Force -Path $assembly,$builtin|Out-Null
            Copy-Item -LiteralPath "$ImportFixtureDirectory\startup-runtime-sxs$bits.exe" -Destination "$app\app.exe"
            Copy-Item -LiteralPath "$ImportFixtureDirectory\native-startup-app$bits.dll" -Destination "$app\wined3d.dll"
            Copy-Item -LiteralPath "$ImportFixtureDirectory\native-startup-private$bits.dll" -Destination "$assembly\wined3d.dll"
            Copy-Item -LiteralPath "$ImportFixtureDirectory\native-startup-builtin$bits.dll" -Destination "$builtin\wined3d.dll"
            [IO.File]::WriteAllText("$assembly\Winedows.Startup.SideBySide.manifest","<?xml version=`"1.0`" encoding=`"UTF-8`"?><assembly xmlns=`"urn:schemas-microsoft-com:asm.v1`" manifestVersion=`"1.0`"><assemblyIdentity type=`"win32`" name=`"Winedows.Startup.SideBySide`" version=`"1.0.0.0`" processorArchitecture=`"$processor`"/><file name=`"wined3d.dll`"/></assembly>")
        }
        foreach($parent in @(32,64)){foreach($bits in @(32,64)){
            $arch=if($bits -eq 32){'x86'}else{'x64'};$builtin="$sxsRuntime\builtin\$arch\wined3d.dll";$assembly=Join-Path $prefix "drive_c\sxs$bits\Winedows.Startup.SideBySide\wined3d.dll"
            Invoke-Case "manifest-wine-handles-$parent-to-$bits" "$sxsRuntime\wine$parent.exe" @('--prefix',$prefix,'run',"C:\sxs$bits\app.exe",'4255494c',$builtin,'builtin') 'wined3d=b'
            Invoke-Case "manifest-native-handles-$parent-to-$bits" "$sxsRuntime\wine$parent.exe" @('--prefix',$prefix,'run',"C:\sxs$bits\app.exe",'50524956',$assembly,'native') 'wined3d=n'
        }}
    }
    $unchanged=$true;foreach($path in $tracked.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $tracked[$path]){$unchanged=$false}}
    @{passed=$unchanged;runtime=$RuntimeDirectory;cases=$cases;fixtureAndProviderFilesUnchanged=$unchanged;noHostSettingsOrInput=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json"
    if(!$unchanged){throw 'Provider or source fixture changed during testing'}
    Write-Host "Selected builtin startup results: $suite"
}catch{@{passed=$false;runtime=$RuntimeDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";throw}
$global:LASTEXITCODE=0
