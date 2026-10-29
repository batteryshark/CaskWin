param([string]$GameExecutable)
$ErrorActionPreference='Stop'
$directory=Join-Path $PSScriptRoot ('test-output\pe-architecture-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory|Out-Null
$source=Join-Path $directory 'bitness.cs'
[IO.File]::WriteAllText($source,'using System; public class Bitness { public static void Main() { Console.WriteLine("bits=" + (IntPtr.Size * 8)); } }')
$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
foreach($fixture in @(@('anycpu','exe','anycpu.exe'),@('x86','exe','x86.exe'),@('anycpu32bitpreferred','exe','preferred.exe'),@('x64','exe','x64.exe'),@('anycpu','library','anycpu.dll'),@('x64','library','x64.dll'))){
    & $csc /nologo "/platform:$($fixture[0])" "/target:$($fixture[1])" "/out:$(Join-Path $directory $fixture[2])" $source
    if($LASTEXITCODE){throw "Cannot compile CLR fixture: $($fixture[2])"}
}
# Alter only test copies, never executing deliberately invalid metadata fixtures.
$mutator=Join-Path $directory 'mutate.py'
@'
import pathlib,struct,sys
root=pathlib.Path(sys.argv[1])
def offsets(data):
    pe=struct.unpack_from('<I',data,0x3c)[0];opt=pe+24
    optional_size=struct.unpack_from('<H',data,pe+20)[0]
    directory=opt+(96 if struct.unpack_from('<H',data,opt)[0]==0x10b else 112)+14*8
    rva=struct.unpack_from('<I',data,directory)[0]
    for i in range(struct.unpack_from('<H',data,pe+6)[0]):
        section=opt+optional_size+i*40
        size,va,rawsize,raw=struct.unpack_from('<IIII',data,section+8)
        if va<=rva<va+max(size,rawsize):return raw+rva-va,directory
    raise ValueError('No CLR header')
data=bytearray((root/'x64.exe').read_bytes());clr,_=offsets(data);flags=struct.unpack_from('<I',data,clr+16)[0];struct.pack_into('<I',data,clr+16,flags|0x20000);(root/'x64-preferred-invalid.exe').write_bytes(data)
data=bytearray((root/'anycpu.exe').read_bytes());clr,directory=offsets(data);flags=struct.unpack_from('<I',data,clr+16)[0];struct.pack_into('<I',data,clr+16,flags&~1);(root/'mixed-metadata.exe').write_bytes(data)
data=bytearray((root/'anycpu.exe').read_bytes());_,directory=offsets(data);struct.pack_into('<I',data,directory,0x7fffffff);(root/'bad-clr-rva.exe').write_bytes(data)
(root/'truncated.exe').write_bytes(b'MZ')
'@ | Set-Content -LiteralPath $mutator
python $mutator $directory
if($LASTEXITCODE){throw 'Cannot prepare architecture metadata fixtures'}
$vs=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'MSVC Build Tools required'}
$env:PE_TEST_VCVARS=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat';$env:PE_TEST_ROOT=$PSScriptRoot;$env:PE_TEST_OUTPUT=$directory
$builder=Join-Path $directory 'build.cmd'
@'
@echo off
call "%PE_TEST_VCVARS%" %1 >nul
if errorlevel 1 exit /b 1
cd /d "%PE_TEST_OUTPUT%"
cl /nologo /std:c++17 /EHsc /MT /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "%PE_TEST_ROOT%\pe-architecture-probe.cpp" "%PE_TEST_ROOT%\pe-architecture.cpp" /Fe:"%PE_TEST_OUTPUT%\pe-probe%2.exe"
'@ | Set-Content -LiteralPath $builder
foreach($arch in @(@('x64',64),@('x64_x86',32))){
    & $env:ComSpec /d /c $builder $arch[0] $arch[1]
    if($LASTEXITCODE){throw 'Architecture probe build failed'}
    $arguments=@($directory);if($GameExecutable){$arguments+=$GameExecutable}
    & (Join-Path $directory "pe-probe$($arch[1]).exe") @arguments|Tee-Object -FilePath (Join-Path $directory "probe$($arch[1]).log")
    if($LASTEXITCODE){throw 'PE architecture validation failed'}
}
foreach($fixture in @(@('anycpu.exe',64),@('x86.exe',32),@('preferred.exe',32),@('x64.exe',64))){
    $observed=& (Join-Path $directory $fixture[0]);if($LASTEXITCODE -or $observed -ne "bits=$($fixture[1])"){throw "Actual CLR architecture differs: $($fixture[0]) => $observed"}
    "$($fixture[0]): $observed"|Tee-Object -FilePath (Join-Path $directory 'actual-clr.log') -Append
}
Write-Host "PE and actual CLR architecture tests passed: $directory"
$global:LASTEXITCODE=0
