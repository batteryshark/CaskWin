param([switch]$SkipBuild,[string]$RuntimeDirectory)
$ErrorActionPreference='Stop'
if(!$RuntimeDirectory){$RuntimeDirectory=Join-Path $PSScriptRoot 'bin'}
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
if(!$SkipBuild){ & "$PSScriptRoot\build.ps1" -OutputDirectory $RuntimeDirectory }
$runtime=Join-Path $RuntimeDirectory 'wine64.exe'
$testRoot=Join-Path $PSScriptRoot ('test-output\suite-'+[Guid]::NewGuid().ToString('N'))
$a=Join-Path $testRoot 'prefix-a'
$b=Join-Path $testRoot 'prefix-b'
$imported=Join-Path $testRoot 'prefix-imported'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$script:passed=0
function Assert-True([bool]$Condition,[string]$Message){if(!$Condition){throw "FAIL: $Message"};$script:passed++;Write-Host "PASS: $Message"}
function Invoke-Wine {param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments) & $runtime @Arguments;if($LASTEXITCODE){throw "Loader returned $LASTEXITCODE for $Arguments"}}
$hostFile=Test-Path -LiteralPath 'C:\wd-tests'
$hostUser=Test-Path -LiteralPath 'HKCU:\Software\WinedowsTests'
$hostMachine=Test-Path -LiteralPath 'HKLM:\Software\WinedowsTests'
Assert-True (!$hostFile -and !$hostUser -and !$hostMachine) 'host test namespaces start empty'
foreach($prefix in @($a,$b)){
    Invoke-Wine @('--prefix',$prefix,'init')
    New-Item -ItemType Directory -Force -Path (Join-Path $prefix 'drive_c\tests'),(Join-Path $prefix 'media\disc'),(Join-Path $prefix 'media\removable') | Out-Null
    Copy-Item -LiteralPath "$RuntimeDirectory\probe64.exe","$RuntimeDirectory\probe32.exe" -Destination (Join-Path $prefix 'drive_c\tests')
    [IO.File]::WriteAllText((Join-Path $prefix 'media\disc\disc.txt'),'virtual disc')
    Invoke-Wine @('--prefix',$prefix,'drive','map','d',(Join-Path $prefix 'media\disc'),'cdrom','TEST_DISC')
    Invoke-Wine @('--prefix',$prefix,'drive','map','e',(Join-Path $prefix 'media\removable'),'removable','TEST_USB')
}
Invoke-Wine @('--prefix',$a,'run','C:\tests\probe64.exe','--cross','C:\tests\probe32.exe')
Invoke-Wine @('--prefix',$a,'run','C:\tests\probe32.exe','--cross','C:\tests\probe64.exe')
Assert-True (Test-Path -LiteralPath (Join-Path $a 'drive_c\wd-tests\child.txt')) 'child file stored in the selected prefix'
Assert-True (!(Test-Path -LiteralPath (Join-Path $a 'media\disc\should-not-exist.txt'))) 'read-only disc remains unchanged'
Invoke-Wine @('--prefix',$b,'run','C:\tests\probe64.exe','--empty')
Assert-True (!(Test-Path -LiteralPath 'C:\wd-tests')) 'no host filesystem test artifacts'
Assert-True (!(Test-Path -LiteralPath 'HKCU:\Software\WinedowsTests')) 'no host HKCU test key'
Assert-True (!(Test-Path -LiteralPath 'HKLM:\Software\WinedowsTests')) 'no host HKLM test key'
Invoke-Wine @('--prefix',$a,'run','C:\tests\probe64.exe','--persist')
New-Item -ItemType Directory -Force -Path $imported | Out-Null
Copy-Item -LiteralPath (Join-Path $a 'user.reg'),(Join-Path $a 'system.reg'),(Join-Path $a 'userdef.reg') -Destination $imported
Invoke-Wine @('--prefix',$imported,'init')
New-Item -ItemType Directory -Force -Path (Join-Path $imported 'drive_c\tests') | Out-Null
Copy-Item -LiteralPath "$RuntimeDirectory\probe64.exe" -Destination (Join-Path $imported 'drive_c\tests')
Invoke-Wine @('--prefix',$imported,'run','C:\tests\probe64.exe','--persist')
Assert-True ((Get-Content -Raw -LiteralPath (Join-Path $imported 'user.reg')).Contains('dword:12345678')) 'Wine text hive round-trip'
# Edit an existing text hive while idle. The next session must import it, not erase it.
$edited=(Get-Content -Raw -LiteralPath (Join-Path $b 'user.reg'))+"`n[Software\\WinedowsTests]`n`"Value`"=dword:12345678`n"
[IO.File]::WriteAllText((Join-Path $b 'user.reg'),$edited)
Invoke-Wine @('--prefix',$b,'run','C:\tests\probe64.exe','--persist')
# Malformed edits must leave both the text input and the previous binary hive intact.
$userFile=Join-Path $b 'user.reg'
$valid=[IO.File]::ReadAllText($userFile)
$invalid=$valid+"`n[Software\\WinedowsTests]`ninvalid-value`n"
[IO.File]::WriteAllText($userFile,$invalid)
& $runtime --prefix $b init
Assert-True ($LASTEXITCODE -ne 0 -and [IO.File]::ReadAllText($userFile) -eq $invalid) 'invalid edited hive is rejected without overwriting input'
[IO.File]::WriteAllText($userFile,$valid)
Invoke-Wine @('--prefix',$b,'run','C:\tests\probe64.exe','--persist')
$bad=Join-Path $testRoot 'invalid-new-prefix'
New-Item -ItemType Directory -Path $bad | Out-Null
[IO.File]::WriteAllText((Join-Path $bad 'user.reg'),$invalid)
& $runtime --prefix $bad init
Assert-True ($LASTEXITCODE -ne 0 -and !(Test-Path -LiteralPath (Join-Path $bad '.winedows\registry.dat'))) 'failed initial import does not leave a partial binary hive'
[IO.File]::WriteAllText((Join-Path $bad 'user.reg'),$valid)
Invoke-Wine @('--prefix',$bad,'init')
Invoke-Wine @('--prefix',$a,'run',$runtime,'gpu')
Write-Host "Suite passed: $passed host assertions plus native x86/x64 probe checks. Artifacts: $testRoot"
@{passed=$true;hostAssertions=$passed;prefixRoot=$testRoot;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json')
