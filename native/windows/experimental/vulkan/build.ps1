param([ValidateSet('x86','x64')][string]$Arch='x64',[string]$StageDirectory=(Join-Path $PSScriptRoot '..\..\test-output\vulkan-experimental'),[string]$WineSourceDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\wine-source.ps1')
$repo=Get-CaskWinWineSource -WineSourceDirectory $WineSourceDirectory
$wine=Assert-CaskWinWineBuild -WineSourceDirectory $repo -Arch $Arch
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory);$generated=Join-Path $StageDirectory 'generated';$out=Join-Path $StageDirectory $Arch
New-Item -ItemType Directory -Force -Path $out|Out-Null
$identity=Get-CaskWinWineSourceIdentity -WineSourceDirectory $repo
if(!(Test-Path -LiteralPath (Join-Path $generated '.caskwin-source-id')) -or (Get-Content -LiteralPath (Join-Path $generated '.caskwin-source-id') -Raw).Trim() -ne $identity){throw 'Vulkan generated headers do not match selected Wine source; run prepare.ps1 first'}
$env:TEMP=$out;$env:TMP=$out
$bin=if($Arch -eq 'x64'){'C:\msys64\mingw64\bin'}else{'C:\msys64\mingw32\bin'}
$env:Path="$bin;$env:Path"
$flags=@('-c','-O1','-g','-D_UCRT','-D__WINESRC__','-D__WINE_PE_BUILD','-DWINEDOWS_NATIVE_VULKAN','-fno-strict-aliasing','-fno-stack-protector','-Wno-format',"-I$generated","-I$repo\dlls\winevulkan","-I$wine\include","-I$repo\include","-I$repo\include\msvcrt")
$objects=@()
foreach($name in @('loader','loader_thunks','vulkan','vulkan_thunks','backend')){
 $source=if($name -eq 'backend'){Join-Path $PSScriptRoot 'backend.c'}else{Join-Path $repo "dlls\winevulkan\$name.c"}
 $object=Join-Path $out "$name.o";$objects+=$object
 $hostFlags=if($name -in @('vulkan','vulkan_thunks','backend')){@('-DWINEDOWS_VULKAN_HOST','-DVK_NO_PROTOTYPES')}else{@()}
 & (Join-Path $bin 'gcc.exe') @flags @hostFlags $source -o $object 2>&1 | Tee-Object -FilePath (Join-Path $out "$name-build.log")
 if($LASTEXITCODE){throw "Compile failed $name"}
}
$resource=Join-Path $out 'winevulkan.res'
$resourceArgs=@('-u','--nostdinc',"--nls-dir=$wine\nls","-I$wine\include","-I$repo\include","-I$repo\include\msvcrt",'-D_UCRT','-D__WINESRC__','-DWINEDOWS_NATIVE_VULKAN','-o',$resource,(Join-Path $repo 'dlls\winevulkan\winevulkan.rc'))|ForEach-Object {$_.Replace('\','/')}
& (Join-Path $wine 'tools\wrc\wrc.exe') @resourceArgs
if($LASTEXITCODE){throw 'Wine Vulkan resource compilation failed'}
$objects+=$resource
$old=Get-Location
try{
 Set-Location $out
 $bits=if($Arch -eq 'x64'){'-m64'}else{'-m32'}
 $libs=@('advapi32','winecrt0','ucrtbase','kernel32','ntdll')|ForEach-Object {Join-Path $wine "dlls\$_\lib$_.a"}
 $linkArgs=@('-o','winevulkan.dll','--wine-objdir',$wine,$bits,'-mno-cygwin','-shared',(Join-Path $repo 'dlls\winevulkan\winevulkan.spec'))+$objects+$libs
 $linkArgs=$linkArgs|ForEach-Object {$_.Replace('\','/')}
 & (Join-Path $wine 'tools\winegcc\winegcc.exe') @linkArgs 2>&1 | Tee-Object -FilePath (Join-Path $out 'link.log')
 if($LASTEXITCODE){throw 'Wine Vulkan link failed'}
}finally{Set-Location $old}
$global:LASTEXITCODE=0
