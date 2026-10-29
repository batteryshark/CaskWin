param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'test-output\runtime08-release'),[string]$FixtureDirectory=(Join-Path $PSScriptRoot 'test-output\startup-builtins-08\fixtures'),[switch]$NativeOnly)
$ErrorActionPreference='Stop';$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory);$FixtureDirectory=[IO.Path]::GetFullPath($FixtureDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\startup-builtins-08\native-bound-'+[Guid]::NewGuid().ToString('N'));$prefix=Join-Path $suite 'prefix'
New-Item -ItemType Directory -Path $suite|Out-Null
$cases=[Collections.Generic.List[object]]::new()
function Copy-BoundFixture([string]$Source,[string]$Destination,[string]$Dependency){
    $bytes=[IO.File]::ReadAllBytes($Source);$nt=[BitConverter]::ToUInt32($bytes,60);$count=[BitConverter]::ToUInt16($bytes,$nt+6);$optional=$nt+24;$sections=$optional+[BitConverter]::ToUInt16($bytes,$nt+20)
    $directory=$optional+$(if([BitConverter]::ToUInt16($bytes,$optional) -eq 0x10b){104}else{120})
    function Offset([uint32]$Rva){
        for($i=0;$i -lt $count;$i++){$section=$sections+$i*40;$virtual=[BitConverter]::ToUInt32($bytes,$section+12);$size=[BitConverter]::ToUInt32($bytes,$section+16);$raw=[BitConverter]::ToUInt32($bytes,$section+20);if($Rva -ge $virtual -and $Rva-$virtual -lt $size){return [int]($raw+$Rva-$virtual)}}
        throw 'Fixture RVA has no section backing'
    }
    $table=Offset ([BitConverter]::ToUInt32($bytes,$directory));$found=$false
    for($i=0;$i -lt 2048;$i++){
        $descriptor=$table+$i*20;$nameRva=[BitConverter]::ToUInt32($bytes,$descriptor+12);if(!$nameRva){break}
        $nameOffset=Offset $nameRva;$end=$nameOffset;while($bytes[$end]){$end++};$name=[Text.Encoding]::ASCII.GetString($bytes,$nameOffset,$end-$nameOffset)
        if($name -ieq $Dependency){[BitConverter]::GetBytes([uint32]0).CopyTo($bytes,$descriptor);[BitConverter]::GetBytes([uint32]0x12345678).CopyTo($bytes,$descriptor+4);$found=$true;break}
    }
    if(!$found){throw "Fixture has no $Dependency import"};[IO.File]::WriteAllBytes($Destination,$bytes)
}
function Invoke-Bound([string]$Name,[string]$Program,[string[]]$Arguments,[string]$Order='',[bool]$Reject=$false){
    $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true;$start.Environment['WINEDLLOVERRIDES']=$Order
    foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($start);$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();$code=124}else{$code=$process.ExitCode}
    $text=$out.GetAwaiter().GetResult()+$err.GetAwaiter().GetResult();$process.Dispose();[IO.File]::WriteAllText("$suite\$Name.log",$text)
    $pass=if($Reject){$code -eq 1 -and $text -match 'bound import has no original name table' -and $text -notmatch 'Provider='}else{$code -eq 0 -and $text -match 'PASS '}
    $cases.Add(@{name=$Name;passed=$pass;exitCode=$code;expectedRejection=$Reject});Write-Host "$Name exit=$code passed=$pass";if(!$pass){Write-Host $text;throw "Native bound import fixture failed: $Name"}
}
try{
    $native=@{}
    foreach($bits in @(32,64)){
        $native[$bits]=(& "$PSScriptRoot\test-output\helpers-08\fixtures\gdiplus-probe$bits.exe" --native-path).Trim();if($LASTEXITCODE){throw 'Native GDI+ discovery failed'}
        Copy-BoundFixture "$FixtureDirectory\builtin-bound-probe$bits.exe" "$suite\unrelated$bits.exe" 'KERNEL32.dll'
        Copy-BoundFixture "$FixtureDirectory\builtin-bound-probe$bits.exe" "$suite\selected$bits.exe" 'gdiplus.dll'
        Copy-BoundFixture "$FixtureDirectory\builtin-child-probe$bits.exe" "$suite\no-match$bits.exe" 'KERNEL32.dll'
        Invoke-Bound "native-unrelated-$bits" "$suite\unrelated$bits.exe" @($native[$bits])
        Invoke-Bound "native-selected-$bits" "$suite\selected$bits.exe" @($native[$bits])
        Invoke-Bound "native-no-match-$bits" "$suite\no-match$bits.exe" @('--noop')
    }
    if(!$NativeOnly){
        $runtime="$RuntimeDirectory\wine64.exe";& $runtime --prefix $prefix init;if($LASTEXITCODE){throw 'Prefix init failed'}
        $app=Join-Path $prefix 'drive_c\bound';New-Item -ItemType Directory -Path $app|Out-Null
        foreach($bits in @(32,64)){
            foreach($kind in @('unrelated','selected','no-match')){Copy-Item -LiteralPath "$suite\$kind$bits.exe" -Destination $app}
            $arch=if($bits -eq 32){'x86'}else{'x64'}
            Invoke-Bound "wine-unrelated-$bits" $runtime @('--prefix',$prefix,'run',"C:\bound\unrelated$bits.exe","$RuntimeDirectory\builtin\$arch\gdiplus.dll") 'gdiplus=b'
            Invoke-Bound "wine-no-match-$bits" $runtime @('--prefix',$prefix,'run',"C:\bound\no-match$bits.exe",'--noop') 'gdiplus=b'
            Invoke-Bound "wine-matching-bound-rejected-$bits" $runtime @('--prefix',$prefix,'run',"C:\bound\selected$bits.exe","$RuntimeDirectory\builtin\$arch\gdiplus.dll") 'gdiplus=b' $true
        }
    }
    @{passed=$true;runtime=$RuntimeDirectory;cases=$cases;nativeBoundDescriptorsExecute=$true}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";Write-Host "Native bound import results: $suite"
}catch{@{passed=$false;runtime=$RuntimeDirectory;cases=$cases;error=$_.Exception.Message}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$suite\result.json";throw}
$global:LASTEXITCODE=0
