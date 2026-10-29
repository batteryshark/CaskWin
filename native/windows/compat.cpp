// SPDX-License-Identifier: LGPL-2.1-or-later
// Host adapter for Wine's portable version APIs and version-dependent loader
// semantics. Version tables/comparison/API code are compiled from upstream.
#define WIN32_NO_STATUS
#include "common.h"
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include "detours.h"
#include <intrin.h>
#include "build/wine_version.inc"

namespace wd {
static decltype(&GetVersion) real_version=GetVersion;
static decltype(&GetVersionExA) real_version_a=GetVersionExA;
static decltype(&GetVersionExW) real_version_w=GetVersionExW;
static decltype(&VerifyVersionInfoA) real_verify_a=VerifyVersionInfoA;
static decltype(&VerifyVersionInfoW) real_verify_w=VerifyVersionInfoW;
static decltype(&LoadLibraryA) real_load_a=LoadLibraryA;
static decltype(&LoadLibraryW) real_load_w=LoadLibraryW;
static decltype(&LoadLibraryExA) real_load_ex_a=LoadLibraryExA;
static decltype(&LoadLibraryExW) real_load_ex_w=LoadLibraryExW;
static decltype(&RegSetValueExA) real_reg_set_a=RegSetValueExA;
using RtlVersion=NTSTATUS(WINAPI*)(RTL_OSVERSIONINFOEXW*);
using RtlVerify=NTSTATUS(WINAPI*)(const RTL_OSVERSIONINFOEXW*,DWORD,DWORDLONG);
using RtlNumbers=void(WINAPI*)(DWORD*,DWORD*,DWORD*);
static RtlVersion real_rtl_version;
static RtlVerify real_rtl_verify;
static RtlNumbers real_rtl_numbers;
static str host_windows;
static bool host_caller(void *address){
    // Unix Wine drivers execute outside the emulated Windows API layer. On
    // this backend the host driver stack is Windows code: preserve its real
    // OS environment instead of making a current GPU driver believe it runs
    // on Windows 98. Application/local/Wine modules retain the configured API.
    DWORD error=GetLastError();HMODULE module=nullptr;wchar_t path[MAX_PATH]{};
    bool host=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)address,&module)
        && GetModuleFileNameW(module,path,MAX_PATH)&&inside(path,host_windows);
    SetLastError(error);return host;
}
static DWORD WINAPI version(){return host_caller(_ReturnAddress())?real_version():wine_version::wine_GetVersion();}
static BOOL WINAPI version_a(OSVERSIONINFOA *info){return host_caller(_ReturnAddress())?real_version_a(info):wine_version::wine_GetVersionExA(info);}
static BOOL WINAPI version_w(OSVERSIONINFOW *info){return host_caller(_ReturnAddress())?real_version_w(info):wine_version::wine_GetVersionExW(info);}
static NTSTATUS WINAPI rtl_version(RTL_OSVERSIONINFOEXW *info){return host_caller(_ReturnAddress())?real_rtl_version(info):wine_version::wine_RtlGetVersion(info);}
static NTSTATUS WINAPI rtl_verify(const RTL_OSVERSIONINFOEXW *info,DWORD types,DWORDLONG conditions){return host_caller(_ReturnAddress())?real_rtl_verify(info,types,conditions):wine_version::wine_RtlVerifyVersionInfo(info,types,conditions);}
static void WINAPI rtl_numbers(DWORD *major,DWORD *minor,DWORD *build){if(host_caller(_ReturnAddress()))real_rtl_numbers(major,minor,build);else wine_version::wine_RtlGetNtVersionNumbers(major,minor,build);}

static bool setting(HKEY root,const str &key,const wchar_t *name,str &value) {
    wchar_t text[256]{};DWORD bytes=sizeof(text);
    LSTATUS e=RegGetValueW(root,key.c_str(),name,RRF_RT_REG_SZ,nullptr,text,&bytes);
    if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return false;
    check(e,"Read Wine compatibility setting");value=text;return true;
}
static BOOL WINAPI verify_w(OSVERSIONINFOEXW *info,DWORD types,DWORDLONG conditions) {
    if(host_caller(_ReturnAddress()))return real_verify_w(info,types,conditions);
    NTSTATUS status=wine_version::wine_RtlVerifyVersionInfo(info,types,conditions);
    if(status==STATUS_SUCCESS)return TRUE;
    SetLastError(status==STATUS_INVALID_PARAMETER?ERROR_BAD_ARGUMENTS:ERROR_OLD_WIN_VERSION);
    return FALSE;
}
static BOOL WINAPI verify_a(OSVERSIONINFOEXA *info,DWORD types,DWORDLONG conditions) {
    if(host_caller(_ReturnAddress()))return real_verify_a(info,types,conditions);
    OSVERSIONINFOEXW value{};value.dwOSVersionInfoSize=sizeof(value);
    value.dwMajorVersion=info->dwMajorVersion;value.dwMinorVersion=info->dwMinorVersion;
    value.dwBuildNumber=info->dwBuildNumber;value.dwPlatformId=info->dwPlatformId;
    value.wServicePackMajor=info->wServicePackMajor;value.wServicePackMinor=info->wServicePackMinor;
    value.wSuiteMask=info->wSuiteMask;value.wProductType=info->wProductType;
    return verify_w(&value,types,conditions);
}
static HMODULE load_result(HMODULE module,DWORD flags) {
    // Same Wine kernelbase/loader.c load_library rule: Win9x's missing-DLL
    // error differs from NT's. Resource/datafile opens do not use that rule.
    if(!module && !(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE))
       && wine_version::current_version->dwPlatformId==VER_PLATFORM_WIN32_WINDOWS
       && GetLastError()==ERROR_MOD_NOT_FOUND)SetLastError(ERROR_DLL_NOT_FOUND);
    return module;
}
static HMODULE WINAPI load_a(LPCSTR name){if(host_caller(_ReturnAddress()))return real_load_a(name);return load_result(real_load_a(name),0);}
static HMODULE WINAPI load_w(LPCWSTR name){if(host_caller(_ReturnAddress()))return real_load_w(name);return load_result(real_load_w(name),0);}
static HMODULE WINAPI load_ex_a(LPCSTR name,HANDLE file,DWORD flags){if(host_caller(_ReturnAddress()))return real_load_ex_a(name,file,flags);return load_result(real_load_ex_a(name,file,flags),flags);}
static HMODULE WINAPI load_ex_w(LPCWSTR name,HANDLE file,DWORD flags){if(host_caller(_ReturnAddress()))return real_load_ex_w(name,file,flags);return load_result(real_load_ex_w(name,file,flags),flags);}
static LSTATUS WINAPI reg_set_a(HKEY key,LPCSTR name,DWORD reserved,DWORD type,const BYTE *data,DWORD count) {
    // Wine dlls/kernelbase/registry.c RegSetValueExA: Windows 9x ignores the
    // supplied REG_SZ count and measures the complete ANSI string itself.
    // Some Win9x games pass strlen(data + 1), truncating every saved string on
    // NT. Keep this rule opt-in through Wine's existing per-application version;
    // native Windows/driver callers and every other value type retain NT rules.
    if(!host_caller(_ReturnAddress())&&type==REG_SZ) {
        if(!data)return ERROR_INVALID_PARAMETER;
        count=(DWORD)(strlen((const char*)data)+1);
    }
    return real_reg_set_a(key,name,reserved,type,data,count);
}

template<class T,class U> static void hook(T &real,U fn){check(DetourAttach((PVOID*)&real,(PVOID)fn),"Attach compatibility API");}
void attach_compatibility(HKEY user) {
    std::vector<wchar_t> path(32768);GetModuleFileNameW(nullptr,path.data(),(DWORD)path.size());
    str application=path.data();application=application.substr(application.find_last_of(L"\\/")+1);
    str configured;
    if(!setting(user,L"Software\\Wine\\AppDefaults\\"+application,L"Version",configured)
       && !setting(user,L"Software\\Wine",L"Version",configured))return;
    // An explicit native setting lets one application opt out of a prefix
    // default, while retaining file/registry isolation and DLL selection.
    if(configured.empty()||!_wcsicmp(configured.c_str(),L"native"))return;
    for(const auto &entry:wine_version::version_names)
        if(!_wcsicmp(configured.c_str(),entry.name))wine_version::current_version=&wine_version::VersionData[entry.ver];
    if(!wine_version::current_version)throw std::runtime_error("Unknown Wine Version setting: "+utf8(configured));
    wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);host_windows=windows;
    HMODULE nt=GetModuleHandleW(L"ntdll.dll");
    real_rtl_version=(RtlVersion)GetProcAddress(nt,"RtlGetVersion");
    real_rtl_verify=(RtlVerify)GetProcAddress(nt,"RtlVerifyVersionInfo");
    real_rtl_numbers=(RtlNumbers)GetProcAddress(nt,"RtlGetNtVersionNumbers");
    if(!real_rtl_version||!real_rtl_verify||!real_rtl_numbers)throw std::runtime_error("Native version APIs unavailable");
    hook(real_version,version);
    hook(real_version_a,version_a);hook(real_version_w,version_w);
    hook(real_rtl_version,rtl_version);hook(real_rtl_verify,rtl_verify);
    hook(real_rtl_numbers,rtl_numbers);
    hook(real_verify_a,verify_a);hook(real_verify_w,verify_w);
    if(wine_version::current_version->dwPlatformId==VER_PLATFORM_WIN32_WINDOWS){
        hook(real_load_a,load_a);hook(real_load_w,load_w);hook(real_load_ex_a,load_ex_a);hook(real_load_ex_w,load_ex_w);
        hook(real_reg_set_a,reg_set_a);
    }
}
}
