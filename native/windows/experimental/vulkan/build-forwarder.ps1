param([ValidateSet('x86','x64')][string]$Arch='x64',[string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\wine-source.ps1')
$repo=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$wine=Assert-CaskWinWineBuild -WineSourceDirectory $repo -Arch $Arch
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$out=Join-Path $StageDirectory $Arch
New-Item -ItemType Directory -Force -Path $out|Out-Null
$env:TEMP=$out;$env:TMP=$out;$bin=if($Arch -eq 'x64'){'C:\msys64\mingw64\bin'}else{'C:\msys64\mingw32\bin'};$env:Path="$bin;$env:Path"
& (Join-Path $bin 'gcc.exe') -c -O2 -D_UCRT -D__WINESRC__ -D__WINE_PE_BUILD -fno-strict-aliasing -fno-stack-protector ("-I$wine\include") ("-I$repo\include") ("-I$repo\include\msvcrt") (Join-Path $repo 'dlls\vulkan-1\vulkan.c') -o (Join-Path $out 'forwarder.o')
if($LASTEXITCODE){throw 'Vulkan forwarding DLL compile failed'}
$resource=Join-Path $out 'version.res'
$resourceArgs=@('-u','--nostdinc',"--nls-dir=$wine\nls","-I$wine\include","-I$repo\include","-I$repo\include\msvcrt",'-D_UCRT','-D__WINESRC__','-o',$resource,(Join-Path $repo 'dlls\vulkan-1\version.rc'))|ForEach-Object {$_.Replace('\','/')}
& (Join-Path $wine 'tools\wrc\wrc.exe') @resourceArgs
if($LASTEXITCODE){throw 'Vulkan forwarder resource compilation failed'}
$old=Get-Location
try{
 Set-Location $out;$bits=if($Arch -eq 'x64'){'-m64'}else{'-m32'}
 $libs=@('user32','winecrt0','ucrtbase','kernel32','ntdll')|ForEach-Object {Join-Path $wine "dlls\$_\lib$_.a"}
 $args=@('-o','vulkan-1.dll','--wine-objdir',$wine,$bits,'-mno-cygwin','-shared',(Join-Path $repo 'dlls\vulkan-1\vulkan-1.spec'),'forwarder.o',$resource)+$libs
 $args=$args|ForEach-Object {$_.Replace('\','/')}
 & (Join-Path $wine 'tools\winegcc\winegcc.exe') @args
 if($LASTEXITCODE){throw 'Vulkan forwarding DLL link failed'}
}finally{Set-Location $old}
$global:LASTEXITCODE=0
