param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\..\bin'),
      [string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),
      [string]$ModuleDirectory)
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
if(!$ModuleDirectory){$ModuleDirectory=Join-Path $RuntimeDirectory 'builtin'}
$ModuleDirectory=[IO.Path]::GetFullPath($ModuleDirectory)
$suite=Join-Path $StageDirectory ('resources-'+[Guid]::NewGuid().ToString('N'))
$prefix=Join-Path $suite 'prefix'
New-Item -ItemType Directory -Force -Path $suite|Out-Null
function Host-Snapshot {
 $result=[ordered]@{}
 foreach($name in @('System32','SysWOW64')){
  $path=Join-Path $env:WINDIR "$name\winevulkan.json"
  $result[$name]=if(Test-Path -LiteralPath $path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{'absent'}
 }
 foreach($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)){
  $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
  $key=$hive.OpenSubKey('Software\Khronos\Vulkan\Drivers')
  $values=[ordered]@{};if($key){foreach($name in ($key.GetValueNames()|Sort-Object)){$values[$name]=@($key.GetValueKind($name).ToString(),$key.GetValue($name))};$key.Dispose()}
  $hive.Dispose();$result[$view.ToString()]=$values
 }
 return ($result|ConvertTo-Json -Depth 5 -Compress)
}
$before=Host-Snapshot
& (Join-Path $RuntimeDirectory 'wine64.exe') --prefix $prefix init
if($LASTEXITCODE){throw 'Resource test prefix initialization failed'}
foreach($name in @('system32','syswow64')){
 $directory=Join-Path $prefix "drive_c\windows\$name"
 New-Item -ItemType Directory -Force -Path $directory|Out-Null
 [IO.File]::WriteAllText((Join-Path $directory 'winevulkan.json'),'WINEDOWS_PRIVATE_REGISTRATION_SENTINEL',[Text.UTF8Encoding]::new($false))
}
$cases=@()
foreach($bits in @(32,64)){
 $arch=if($bits -eq 32){'x86'}else{'x64'};$bin="C:\msys64\mingw$bits\bin"
 $env:Path="$bin;$env:Path";$env:TEMP=$suite;$env:TMP=$suite
 $output=Join-Path $suite "resource-probe$bits.exe"
 & (Join-Path $bin 'g++.exe') -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -municode (Join-Path $PSScriptRoot 'resource-probe.cpp') -o $output -lversion -ladvapi32
 if($LASTEXITCODE){throw "Resource probe build failed $bits"}
 & (Join-Path $RuntimeDirectory 'wine64.exe') --prefix $prefix run $output (Join-Path $ModuleDirectory "$arch\winevulkan.dll") (Join-Path $ModuleDirectory "$arch\vulkan-1.dll") 2>&1 | Tee-Object -FilePath (Join-Path $suite "resource$bits.log")
 $passed=$LASTEXITCODE -eq 0;$cases+=@{architecture=$arch;passed=$passed}
 if(!$passed){throw "Resource/registration fixture failed $bits"}
}
$after=Host-Snapshot
if($after -ne $before){throw 'Host Vulkan manifest/ICD registry snapshot changed'}
@{passed=$true;hostStatePreserved=$true;cases=$cases}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
$global:LASTEXITCODE=0
