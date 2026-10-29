param([string]$RuntimeDirectory=(Join-Path $PSScriptRoot 'bin'))
$ErrorActionPreference='Stop'
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$suite=Join-Path $PSScriptRoot ('test-output\prefix-native-dll-'+[Guid]::NewGuid().ToString('N'))
$runtimeDirectoryCopy=Join-Path $suite 'runtime'
$prefix=Join-Path $suite 'prefix'
$build=Join-Path $suite 'build'
New-Item -ItemType Directory -Path $runtimeDirectoryCopy,$build|Out-Null
foreach($name in @('wine64.exe','wine32.exe','winedows64.dll','winedows32.dll')){
    Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $name) -Destination $runtimeDirectoryCopy
}
$runtime=Join-Path $runtimeDirectoryCopy 'wine64.exe'
$source=Join-Path $build 'native-dll-probe.cpp'
[IO.File]::WriteAllText($source,@'
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
static bool file_id(const wchar_t *path,BY_HANDLE_FILE_INFORMATION &info) {
    HANDLE h=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    bool ok=h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h,&info);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return ok;
}
static bool matches(const wchar_t *path,const BY_HANDLE_FILE_INFORMATION &expected) {
    BY_HANDLE_FILE_INFORMATION actual{};
    return file_id(path,actual)&&actual.dwVolumeSerialNumber==expected.dwVolumeSerialNumber&&
        actual.nFileIndexHigh==expected.nFileIndexHigh&&actual.nFileIndexLow==expected.nFileIndexLow;
}
static bool same_file(const wchar_t *left,const wchar_t *right) {
    HANDLE a=CreateFileW(left,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(right,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    bool same=a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y)&&
        x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);return same;
}
static DWORD WINAPI watchdog(void *){Sleep(30000);TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    if(argc==7&&!wcscmp(argv[1],L"--views")){
        BY_HANDLE_FILE_INFORMATION wow{},native{},except{};
        if(!file_id(argv[3],wow)||!file_id(argv[4],native)||!file_id(argv[6],except))return 2;
        wchar_t windows[32768]{};GetWindowsDirectoryW(windows,32768);
        std::wstring root=windows,system=root+L"\\System32\\"+argv[2],explicit_wow=root+L"\\SysWOW64\\"+argv[2],sysnative=root+L"\\Sysnative\\"+argv[2];
        unsigned failures=0;auto check=[&](bool ok,const char *name){printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
        check(matches(system.c_str(),wow),"x86 System32 selects WOW64 file");
        check(matches(explicit_wow.c_str(),wow),"explicit SysWOW64 selects WOW64 file");
        check(matches(sysnative.c_str(),native),"Sysnative selects native file");
        check(matches((root+L"\\System32\\drivers\\etc\\"+argv[5]).c_str(),except),"drivers\\etc remains a native redirection exception");
        PVOID previous=nullptr;BOOL disabled=Wow64DisableWow64FsRedirection(&previous);
        check(disabled!=FALSE,"disable thread filesystem redirection");
        if(disabled){
            check(matches(system.c_str(),native),"disabled System32 selects native file");
            check(matches(explicit_wow.c_str(),wow),"disabled explicit SysWOW64 stays WOW64");
            check(Wow64RevertWow64FsRedirection(previous)!=FALSE,"restore thread filesystem redirection");
            check(matches(system.c_str(),wow),"reverted System32 selects WOW64 file again");
        }
        return failures?1:0;
    }
    if(argc!=2)return 2;
    HMODULE module=LoadLibraryW(L"wined3d.dll");DWORD error=GetLastError();
    if(!wcscmp(argv[1],L"--absent")){
        bool absent=!module&&(error==ERROR_MOD_NOT_FOUND||error==ERROR_FILE_NOT_FOUND);
        printf("%s missing strict native provider (error=%lu)\n",absent?"PASS":"FAIL",error);
        if(module)FreeLibrary(module);return absent?0:1;
    }
    if(!module){printf("FAIL load marker provider (error=%lu)\n",error);return 1;}
    wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);
    auto marker=(int(*)())GetProcAddress(module,"LoaderPathMarker");
    bool ok=marker&&marker()==0x5744504d&&same_file(path,argv[1]);
    wprintf(L"%ls actual provider file identity: %ls; expected: %ls\n",ok?L"PASS":L"FAIL",path,argv[1]);
    FreeLibrary(module);return ok?0:1;
}
'@)
$savedPath=$env:PATH;$savedTemp=$env:TEMP;$savedTmp=$env:TMP;$savedOverrides=$env:WINEDLLOVERRIDES
$cases=[Collections.Generic.List[object]]::new()
function Invoke-NativeFixture([int]$bits,[string]$label,[string]$order,[string]$program,[string]$expected,[string]$selected){
    $env:WINEDLLOVERRIDES="wined3d=$order"
    $log=Join-Path $suite "$bits-$label.log"
    & $runtime --prefix $prefix run $program $expected 2>&1|Tee-Object -FilePath $log
    $code=$LASTEXITCODE;$text=Get-Content -Raw -LiteralPath $log
    $selection=if($selected){$text.Contains("DLL wined3d.dll: $selected ($expected)")}else{$text -notmatch 'DLL wined3d.dll:'}
    $cases.Add(@{architecture=$bits;fixture=$label;order=$order;expected=$expected;exitCode=$code;selectionMatches=$selection;passed=(!$code -and $selection)})
    if($code -or !$selection){throw "Private native DLL regression failed: $bits-$label"}
}
try {
    $env:WINEDLLOVERRIDES=$null
    & $runtime --prefix $prefix init
    if($LASTEXITCODE){throw 'Cannot initialize private-native DLL prefix'}
    foreach($bits in @(32,64)){
        $arch=if($bits -eq 32){'x86'}else{'x64'}
        $toolchain=if($bits -eq 32){'C:\msys64\mingw32\bin'}else{'C:\msys64\mingw64\bin'}
        $target=if($bits -eq 32){'i686-w64-mingw32'}else{'x86_64-w64-mingw32'}
        $env:PATH="$toolchain;$savedPath";$env:TEMP=$build;$env:TMP=$build
        $compiler=Join-Path $toolchain 'g++.exe'
        if((& $compiler -dumpmachine) -ne $target){throw "Wrong $arch compiler target"}
        $probe=Join-Path $build "native-dll-probe$bits.exe"
        & $compiler -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type -Wno-misleading-indentation -static -static-libgcc -static-libstdc++ -municode $source -o $probe
        if($LASTEXITCODE){throw "Cannot build private-native $arch probe"}
        $imports=& (Join-Path $toolchain 'objdump.exe') -p $probe|Select-String 'DLL Name:'
        if($LASTEXITCODE -or $imports -match 'libgcc_s|libstdc\+\+|libwinpthread|msys|cygwin'){throw 'Probe has unexpected runtime dependencies'}
        $marker=Join-Path $PSScriptRoot "bin\loader-paths-marker$bits.dll"
        $app=Join-Path $prefix "drive_c\tests$bits"
        $system=Join-Path $prefix $(if($bits -eq 32){'drive_c\windows\SysWOW64'}else{'drive_c\windows\System32'})
        $bundle=Join-Path $runtimeDirectoryCopy "builtin\$arch"
        New-Item -ItemType Directory -Force -Path $app,$system,$bundle|Out-Null
        Copy-Item -LiteralPath $probe -Destination $app
        $program="C:\tests$bits\native-dll-probe$bits.exe"
        $privateDll=Join-Path $system 'wined3d.dll'
        $builtinDll=Join-Path $bundle 'wined3d.dll'
        Copy-Item -LiteralPath $marker -Destination $privateDll
        Copy-Item -LiteralPath $marker -Destination $builtinDll
        Invoke-NativeFixture $bits 'private-native-first' 'n,b' $program $privateDll 'native'
        Invoke-NativeFixture $bits 'private-strict-native' 'n' $program $privateDll 'native'
        Invoke-NativeFixture $bits 'builtin-first' 'b,n' $program $builtinDll 'Wine builtin'
        $appDll=Join-Path $app 'wined3d.dll'
        Copy-Item -LiteralPath $marker -Destination $appDll
        Invoke-NativeFixture $bits 'app-before-private-system' 'n,b' $program $appDll 'native'
        Remove-Item -LiteralPath $appDll
        # An external application directory can itself have a private overlay.
        # Both sides are harmless files below this suite, never game files.
        $hostApp=Join-Path $suite "host-app$bits"
        New-Item -ItemType Directory -Path $hostApp|Out-Null
        Copy-Item -LiteralPath $probe -Destination $hostApp
        $hostAppDll=Join-Path $hostApp 'wined3d.dll'
        Copy-Item -LiteralPath $marker -Destination $hostAppDll
        $shadowDll=& $runtime --prefix $prefix path $hostAppDll
        if($LASTEXITCODE -or !$shadowDll -or $shadowDll -eq $hostAppDll){throw 'Cannot resolve private app overlay fixture'}
        New-Item -ItemType Directory -Force -Path (Split-Path $shadowDll)|Out-Null
        Copy-Item -LiteralPath $marker -Destination $shadowDll
        $hostProgram=Join-Path $hostApp "native-dll-probe$bits.exe"
        Invoke-NativeFixture $bits 'private-app-overlay-first' 'n,b' $hostProgram $shadowDll 'native'
        Remove-Item -LiteralPath $shadowDll
        Invoke-NativeFixture $bits 'host-app-read-through' 'n,b' $hostProgram $hostAppDll 'native'
        Remove-Item -LiteralPath $privateDll
        $hostDll=Join-Path $env:WINDIR $(if($bits -eq 32){'SysWOW64\wined3d.dll'}else{'System32\wined3d.dll'})
        if(!(Test-Path -LiteralPath $hostDll)){
            Invoke-NativeFixture $bits 'missing-native-builtin-fallback' 'n,b' $program $builtinDll 'Wine builtin'
            Invoke-NativeFixture $bits 'missing-strict-native' 'n' $program '--absent' ''
        }else{$cases.Add(@{architecture=$bits;fixture='missing-native';status='unavailable';reason='Host supplies wined3d.dll'})}
    }
    # Verify the same real WOW64 routing contract outside and inside the prefix.
    $probe32=Join-Path $build 'native-dll-probe32.exe'
    & $probe32 --views ntdll.dll "$env:WINDIR\SysWOW64\ntdll.dll" "$env:WINDIR\Sysnative\ntdll.dll" hosts "$env:WINDIR\Sysnative\drivers\etc\hosts" 2>&1|Tee-Object -FilePath (Join-Path $suite 'native-system-views.log')
    if($LASTEXITCODE){throw 'Host WOW64 reference behavior differs from fixture expectations'}
    $private32=Join-Path $prefix 'drive_c\windows\SysWOW64\wined3d.dll'
    $private64=Join-Path $prefix 'drive_c\windows\System32\wined3d.dll'
    Copy-Item -LiteralPath "$PSScriptRoot\bin\loader-paths-marker32.dll" -Destination $private32
    Copy-Item -LiteralPath "$PSScriptRoot\bin\loader-paths-marker64.dll" -Destination $private64
    $exception=Join-Path $prefix 'drive_c\windows\System32\drivers\etc\winedows-view-marker.txt'
    $wrongException=Join-Path $prefix 'drive_c\windows\SysWOW64\drivers\etc\winedows-view-marker.txt'
    foreach($file in @($exception,$wrongException)){New-Item -ItemType Directory -Force -Path (Split-Path $file)|Out-Null;[IO.File]::WriteAllText($file,$file)}
    $env:WINEDLLOVERRIDES=$null
    & $runtime --prefix $prefix run 'C:\tests32\native-dll-probe32.exe' --views wined3d.dll $private32 $private64 winedows-view-marker.txt $exception 2>&1|Tee-Object -FilePath (Join-Path $suite 'private-system-views.log')
    $viewCode=$LASTEXITCODE;$cases.Add(@{architecture=32;fixture='system-views-match-native';exitCode=$viewCode;passed=(!$viewCode)})
    if($viewCode){throw 'Private WOW64 system-file routing differs from host reference behavior'}
    @{passed=$true;runtimeSource=$RuntimeDirectory;cases=$cases;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    Write-Host "Private native DLL suite passed: $suite"
}catch{
    @{passed=$false;runtimeSource=$RuntimeDirectory;error=$_.Exception.Message;cases=$cases;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $suite 'result.json')
    throw
}finally{$env:PATH=$savedPath;$env:TEMP=$savedTemp;$env:TMP=$savedTmp;$env:WINEDLLOVERRIDES=$savedOverrides}
$global:LASTEXITCODE=0
