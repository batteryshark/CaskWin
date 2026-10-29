param([string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
. (Join-Path $PSScriptRoot '..\..\wine-source.ps1')
$WineSourceDirectory=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$identity=Get-CaskWinWineSourceIdentity -WineSourceDirectory $WineSourceDirectory
$generated=Join-Path $StageDirectory 'generated'
# The standalone MinGW probe needs just this header, without Wine's CRT headers.
# The DLL is compiled directly from reviewed Wine sources: no substitutions.
$header=[IO.File]::ReadAllBytes((Join-Path $WineSourceDirectory 'include\wine\vulkan.h'))
$source=[IO.File]::ReadAllText((Join-Path $WineSourceDirectory 'include\wine\vulkan_driver.h'))
$driverSection=($source -split 'struct vulkan_funcs\r?\n\{')[1] -split '\r?\n\};' | Select-Object -First 1
if(!$driverSection){throw 'Wine Vulkan driver table was not found'}
$entries=[regex]::Matches($driverSection,'PFN_(vk\w+) p_\1;')|ForEach-Object {'USE_NATIVE_ENTRY('+$_.Groups[1].Value+')'}
if($entries.Count -lt 20){throw 'Wine Vulkan driver table extraction is incomplete'}
New-Item -ItemType Directory -Force -Path $generated,(Join-Path $generated 'wine')|Out-Null
$stamp=Join-Path $generated '.caskwin-source-id'
if(Test-Path -LiteralPath $stamp){Remove-Item -LiteralPath $stamp}
[IO.File]::WriteAllBytes((Join-Path $generated 'wine\vulkan.h'),$header)
[IO.File]::WriteAllText((Join-Path $generated 'native-entries.h'),($entries -join [Environment]::NewLine),[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($stamp,$identity,[Text.UTF8Encoding]::new($false))
