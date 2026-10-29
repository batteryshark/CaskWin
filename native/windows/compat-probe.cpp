// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstdlib>
static int failures;
static void check(bool result,const char *name){printf("%s %s (error=%lu)\n",result?"PASS":"FAIL",name,GetLastError());if(!result)++failures;}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    OSVERSIONINFOEXW w{};w.dwOSVersionInfoSize=sizeof(w);
    OSVERSIONINFOEXA a{};a.dwOSVersionInfoSize=sizeof(a);
    OSVERSIONINFOEXW nt{};nt.dwOSVersionInfoSize=sizeof(nt);
    using RtlVersion=LONG(WINAPI*)(OSVERSIONINFOEXW*);
    auto rtl=(RtlVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion");
    check(GetVersionExW((OSVERSIONINFOW*)&w)&&GetVersionExA((OSVERSIONINFOA*)&a)&&rtl&&rtl(&nt)==0,"version queries");
    DWORD packed=GetVersion();
    printf("VERSION %lu %lu %lu %lu %u %lu %lu %lu %lu %lu\n",w.dwMajorVersion,w.dwMinorVersion,w.dwBuildNumber,w.dwPlatformId,w.wServicePackMajor,packed,nt.dwMajorVersion,nt.dwMinorVersion,nt.dwBuildNumber,nt.dwPlatformId);
    if(argc!=6)return failures?1:0;
    DWORD major=wcstoul(argv[1],nullptr,0),minor=wcstoul(argv[2],nullptr,0),build=wcstoul(argv[3],nullptr,0),platform=wcstoul(argv[4],nullptr,0),sp=wcstoul(argv[5],nullptr,0);
    check(w.dwMajorVersion==major&&w.dwMinorVersion==minor&&w.dwBuildNumber==build&&w.dwPlatformId==platform&&w.wServicePackMajor==sp,"configured Wine version");
    check(a.dwMajorVersion==major&&a.dwMinorVersion==minor&&a.dwBuildNumber==build&&a.dwPlatformId==platform&&a.wServicePackMajor==sp,"ANSI version agrees");
    check(nt.dwMajorVersion==major&&nt.dwMinorVersion==minor&&nt.dwBuildNumber==build&&nt.dwPlatformId==platform,"NT version agrees");
    check(LOBYTE(LOWORD(packed))==major&&HIBYTE(LOWORD(packed))==minor&&((packed&0x80000000)!=0)==(platform!=VER_PLATFORM_WIN32_NT),"packed GetVersion and Win9x flag");
    OSVERSIONINFOEXW request=w;
    ULONGLONG condition=VerSetConditionMask(0,VER_MAJORVERSION,VER_EQUAL);
    condition=VerSetConditionMask(condition,VER_MINORVERSION,VER_EQUAL);
    check(VerifyVersionInfoW(&request,VER_MAJORVERSION|VER_MINORVERSION,condition)!=FALSE,"version equality");
    request.dwMinorVersion++;
    check(!VerifyVersionInfoW(&request,VER_MAJORVERSION|VER_MINORVERSION,condition)&&GetLastError()==ERROR_OLD_WIN_VERSION,"version mismatch error");
    request.dwMajorVersion=major-1;request.dwMinorVersion=99;
    condition=VerSetConditionMask(0,VER_MAJORVERSION,VER_GREATER_EQUAL);
    condition=VerSetConditionMask(condition,VER_MINORVERSION,VER_GREATER_EQUAL);
    check(VerifyVersionInfoW(&request,VER_MAJORVERSION|VER_MINORVERSION,condition)!=FALSE,"major version takes priority over minor comparison");
    a.dwMajorVersion=major;
    check(VerifyVersionInfoA(&a,VER_MAJORVERSION,VerSetConditionMask(0,VER_MAJORVERSION,VER_EQUAL))!=FALSE,"ANSI version verification");
    check(!VerifyVersionInfoW(&request,0,0)&&GetLastError()==ERROR_BAD_ARGUMENTS,"invalid verification condition");
    OSVERSIONINFOA short_a{};short_a.dwOSVersionInfoSize=sizeof(short_a);
    OSVERSIONINFOW short_w{};short_w.dwOSVersionInfoSize=sizeof(short_w);
    check(GetVersionExA(&short_a)&&GetVersionExW(&short_w)&&short_a.dwMajorVersion==major&&short_w.dwMajorVersion==major,"short version structures");
    short_a.dwOSVersionInfoSize=0;
    check(!GetVersionExA(&short_a)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER,"invalid ANSI structure matches Wine");
    DWORD missing_error=platform==VER_PLATFORM_WIN32_WINDOWS?ERROR_DLL_NOT_FOUND:ERROR_MOD_NOT_FOUND;
    check(!LoadLibraryA("winedows-intentionally-missing-4e6a.dll")&&GetLastError()==missing_error,"ANSI missing-DLL semantics");
    check(!LoadLibraryW(L"winedows-intentionally-missing-4e6a.dll")&&GetLastError()==missing_error,"Unicode missing-DLL semantics");
    check(!LoadLibraryExA("winedows-intentionally-missing-4e6a.dll",nullptr,0)&&GetLastError()==missing_error,"ANSI extended missing-DLL semantics");
    check(!LoadLibraryExW(L"winedows-intentionally-missing-4e6a.dll",nullptr,0)&&GetLastError()==missing_error,"Unicode extended missing-DLL semantics");
    check(!LoadLibraryExW(L"winedows-intentionally-missing-4e6a.dll",nullptr,LOAD_LIBRARY_AS_DATAFILE)&&GetLastError()!=ERROR_DLL_NOT_FOUND,"datafile load preserves its native error");
    HMODULE module=LoadLibraryW(L"version.dll");check(module!=nullptr,"successful native load is preserved");if(module)FreeLibrary(module);
    printf("Version compatibility %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);
    return failures?1:0;
}
