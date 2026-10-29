// SPDX-License-Identifier: LGPL-2.1-or-later
// Preserve the launcher's Wine provider selection for explicit DLL paths too.
#include "module-runtime.h"
#include "builtin-catalog.h"
#include "detours.h"
#include <cwchar>
#include <cstring>
#include <psapi.h>
#include <map>
#include <cstdint>

namespace wd {
namespace {
struct Provider {str name,path;HMODULE module=nullptr;};
Provider providers[sizeof(portable_builtins)/sizeof(portable_builtins[0])];
std::vector<str> disabled_modules;
// Windows' second LdrLoadDll argument is a pointer, unlike Wine's internal ABI.
using LoadDll=NTSTATUS(NTAPI*)(PCWSTR,PULONG,const UNICODE_STRING*,HMODULE*);
using GetDll=NTSTATUS(NTAPI*)(PCWSTR,PULONG,const UNICODE_STRING*,HMODULE*);
using GetDllEx=NTSTATUS(NTAPI*)(ULONG,PCWSTR,PULONG,const UNICODE_STRING*,HMODULE*);
LoadDll real_load;
GetDll real_get;
GetDllEx real_get_ex;
using CloseObject=NTSTATUS(NTAPI*)(HANDLE);
using DuplicateObject=NTSTATUS(NTAPI*)(HANDLE,HANDLE,HANDLE,PHANDLE,ACCESS_MASK,ULONG,ULONG);
CloseObject real_close_object;
DuplicateObject real_duplicate_object;
decltype(&CompareObjectHandles) compare_object_handles;
decltype(&LoadLibraryExW) real_load_ex_w=LoadLibraryExW;
decltype(&LoadLibraryExA) real_load_ex_a=LoadLibraryExA;
thread_local unsigned resource_load;
thread_local unsigned system_backend_load;
struct PreserveError {DWORD value=GetLastError();~PreserveError(){SetLastError(value);}};
SRWLOCK alias_lock=SRWLOCK_INIT;
// Metadata only: retaining a duplicate file handle here would extend sharing
// locks and delay deletion. Generations prevent a late close from erasing a
// newly reused handle's record. Kernel handle transfers across processes are
// outside this requested-name cache; normal loaders open their own DLL files.
auto &aliases=*new std::map<HANDLE,uint64_t>;
uint64_t alias_generation_counter;
struct AliasLock {
    bool exclusive;
    explicit AliasLock(bool write):exclusive(write){if(write)AcquireSRWLockExclusive(&alias_lock);else AcquireSRWLockShared(&alias_lock);}
    ~AliasLock(){if(exclusive)ReleaseSRWLockExclusive(&alias_lock);else ReleaseSRWLockShared(&alias_lock);}
};
uint64_t alias_generation(HANDLE file) {
    AliasLock lock(false);auto found=aliases.find(file);return found==aliases.end()?0:found->second;
}
void erase_alias(HANDLE file,uint64_t generation) {
    if(!generation)return;AliasLock lock(true);auto found=aliases.find(file);
    if(found!=aliases.end()&&found->second==generation)aliases.erase(found);
}
NTSTATUS record_alias(HANDLE file,bool disabled) noexcept {
    try{
        AliasLock lock(true);
        if(!disabled){aliases.erase(file);return 0;}
        if((aliases.size()>=4096&&!aliases.count(file))||alias_generation_counter==UINT64_MAX)return (NTSTATUS)0xc000009a;
        aliases[file]=++alias_generation_counter;return 0;
    }catch(...){return (NTSTATUS)0xc0000017;}
}
NTSTATUS NTAPI close_object(HANDLE file) {
    PreserveError error;uint64_t generation=alias_generation(file);
    NTSTATUS status=real_close_object(file);
    if(status>=0)erase_alias(file,generation);
    return status;
}
NTSTATUS NTAPI duplicate_object(HANDLE source_process,HANDLE source,HANDLE target_process,PHANDLE target,ACCESS_MASK access,ULONG attributes,ULONG options) {
    PreserveError error;
    auto local=[](HANDLE process){return process&&(process==GetCurrentProcess()||compare_object_handles(process,GetCurrentProcess()));};
    bool local_source=local(source_process),local_target=local(target_process);
    uint64_t generation=local_source?alias_generation(source):0;
    DWORD flags=0;bool close_source=generation&&(options&DUPLICATE_CLOSE_SOURCE)
        &&(!GetHandleInformation(source,&flags)||!(flags&HANDLE_FLAG_PROTECT_FROM_CLOSE));
    NTSTATUS status=real_duplicate_object(source_process,source,target_process,target,access,attributes,options);
    if(status>=0&&local_target&&target){
        NTSTATUS tracked=record_alias(*target,generation!=0);
        if(tracked<0){real_close_object(*target);*target=nullptr;status=tracked;}
    }
    // Native duplication closes an unprotected source even on failure.
    if(close_source)erase_alias(source,generation);
    return status;
}

bool disabled_name(const wchar_t *name,size_t count) {
    if(resource_load||system_backend_load||disabled_modules.empty()||!name||!count)return false;
    size_t start=0;
    for(size_t i=0;i<count;i++)if(name[i]==L'\\'||name[i]==L'/'||name[i]==L':')start=i+1;
    name+=start;count-=start;
    // Strip exactly one DLL suffix, as Wine's load-order lookup does. A
    // descriptor for foo.dll.dll must not also disable the distinct foo.dll.
    if(count>=4&&!_wcsnicmp(name+count-4,L".dll",4))count-=4;
    for(const auto &disabled:disabled_modules){
        size_t size=disabled.size();
        if(size==count+4&&!_wcsnicmp(name,disabled.c_str(),count))return true;
    }
    return false;
}

Provider *selected(const wchar_t *name,size_t count) {
    if(!name||!count)return nullptr;
    size_t start=0;
    for(size_t i=0;i<count;i++)if(name[i]==L'\\'||name[i]==L'/'||name[i]==L':')start=i+1;
    name+=start;count-=start;
    for(auto &provider:providers){
        if(!provider.module)continue;
        size_t size=provider.name.size();
        // Windows supplies .dll for an extensionless library request.
        if((count==size||count==size-4)&&!_wcsnicmp(name,provider.name.c_str(),count))return &provider;
    }
    return nullptr;
}
const UNICODE_STRING *redirect(const UNICODE_STRING *name,UNICODE_STRING &mapped) {
    if(resource_load||system_backend_load||!name||name->Length%sizeof(wchar_t))return name;
    auto provider=selected(name->Buffer,name->Length/sizeof(wchar_t));
    if(!provider)return name;
    mapped.Buffer=const_cast<wchar_t*>(provider->path.c_str());
    mapped.Length=(USHORT)(provider->path.size()*sizeof(wchar_t));
    mapped.MaximumLength=mapped.Length+sizeof(wchar_t);
    return &mapped;
}
NTSTATUS NTAPI load(PCWSTR path,PULONG characteristics,const UNICODE_STRING *name,HMODULE *out) {
    if(name&&!(name->Length%sizeof(wchar_t))&&disabled_name(name->Buffer,name->Length/sizeof(wchar_t))){
        // Wine reuses an existing module before consulting its load order.
        // Keep the real load for that case to preserve reference counts and
        // argument validation; do not hide trusted backend module handles.
        HMODULE existing=nullptr;
        if(real_get_ex(0,path,characteristics,name,&existing)<0){
            if(out)*out=nullptr;
            return (NTSTATUS)0xc0000135; // STATUS_DLL_NOT_FOUND
        }
        // Hold a temporary reference across the real load so a concurrent
        // last FreeLibrary cannot turn reuse into a fresh disabled mapping.
        NTSTATUS status=real_load(path,characteristics,name,out);
        DWORD error=GetLastError();FreeLibrary(existing);SetLastError(error);
        return status;
    }
    UNICODE_STRING mapped{};
    // Calling the loader, rather than returning the preload handle directly,
    // preserves reference counts and its validation/error handling.
    return real_load(path,characteristics,redirect(name,mapped),out);
}
NTSTATUS NTAPI get(PCWSTR path,PULONG characteristics,const UNICODE_STRING *name,HMODULE *out) {
    UNICODE_STRING mapped{};return real_get(path,characteristics,redirect(name,mapped),out);
}
NTSTATUS NTAPI get_ex(ULONG flags,PCWSTR path,PULONG characteristics,const UNICODE_STRING *name,HMODULE *out) {
    UNICODE_STRING mapped{};return real_get_ex(flags,path,characteristics,redirect(name,mapped),out);
}
struct ResourceLoad {
    bool skip;
    explicit ResourceLoad(DWORD flags):skip(!!(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE))){if(skip)++resource_load;}
    ~ResourceLoad(){if(skip)--resource_load;}
};
HMODULE WINAPI load_ex_w(LPCWSTR path,HANDLE file,DWORD flags){ResourceLoad scope(flags);return real_load_ex_w(path,file,flags);}
HMODULE WINAPI load_ex_a(LPCSTR path,HANDLE file,DWORD flags){ResourceLoad scope(flags);return real_load_ex_a(path,file,flags);}
template<class T> void resolve(T &function,HMODULE module,const char *name) {
    function=(T)GetProcAddress(module,name);
    if(!function)throw std::runtime_error(std::string("Missing module loader export: ")+name);
}
template<class T,class U> void attach(T &function,U hook){check(DetourAttach((PVOID*)&function,(PVOID)hook),"Attach module provider hook");}
}
HMODULE selected_builtin_module(const wchar_t *basename) {
    auto provider=basename?selected(basename,wcslen(basename)):nullptr;
    return provider?provider->module:nullptr;
}
SystemBackendScope::SystemBackendScope(){++system_backend_load;}
SystemBackendScope::~SystemBackendScope(){--system_backend_load;}
static bool same_system_file(const wchar_t *expected,HMODULE module,DWORD &error) {
    // The loader may preserve the logical System32 spelling on WOW64. Query
    // the mapped image's backing file instead, then compare file identities.
    std::vector<wchar_t> mapped(32768);
    DWORD capacity=(DWORD)mapped.size()-1;
    DWORD count=GetMappedFileNameW(GetCurrentProcess(),module,mapped.data(),capacity);
    if(!count||count>=capacity){error=count?ERROR_INSUFFICIENT_BUFFER:GetLastError();return false;}
    str actual=L"\\\\?\\GLOBALROOT"+str(mapped.data(),count);
    HANDLE expected_file=CreateFileW(expected,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                     nullptr,OPEN_EXISTING,0,nullptr);
    if(expected_file==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    HANDLE actual_file=CreateFileW(actual.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                   nullptr,OPEN_EXISTING,0,nullptr);
    if(actual_file==INVALID_HANDLE_VALUE){error=GetLastError();CloseHandle(expected_file);return false;}
    FILE_ID_INFO expected_id{},actual_id{};
    bool queried=GetFileInformationByHandleEx(expected_file,FileIdInfo,&expected_id,sizeof(expected_id))
                 &&GetFileInformationByHandleEx(actual_file,FileIdInfo,&actual_id,sizeof(actual_id));
    if(!queried)error=GetLastError();
    else if(expected_id.VolumeSerialNumber!=actual_id.VolumeSerialNumber
            ||memcmp(expected_id.FileId.Identifier,actual_id.FileId.Identifier,sizeof(expected_id.FileId.Identifier)))
        error=ERROR_INVALID_DLL;
    else error=ERROR_SUCCESS;
    CloseHandle(actual_file);CloseHandle(expected_file);
    return error==ERROR_SUCCESS;
}
static HMODULE load_fixed_system_library(const wchar_t *name) {
    std::vector<wchar_t> path(32768);
    // The WOW64 implementation uses a USHORT byte length: 32768 WCHARs wraps
    // that size to zero and returns a required size without filling the buffer.
    UINT capacity=(UINT)path.size()-1;
    UINT size=sizeof(void*)==4?GetSystemWow64DirectoryW(path.data(),capacity):0;
    if(!size)size=GetSystemDirectoryW(path.data(),capacity);
    if(!size||size>=capacity||!path[0]||size+wcslen(name)+2>path.size()){SetLastError(ERROR_INSUFFICIENT_BUFFER);return nullptr;}
    wcscat_s(path.data(),path.size(),L"\\");wcscat_s(path.data(),path.size(),name);
    SystemBackendScope scope;
    HMODULE module=real_load_ex_w(path.data(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD load_error=GetLastError();
    if(!module){SetLastError(load_error);return nullptr;}
    DWORD error=ERROR_SUCCESS;
    bool valid;
    try {valid=same_system_file(path.data(),module,error);}
    catch(...){FreeLibrary(module);throw;}
    if(!valid){
        FreeLibrary(module);SetLastError(error?error:ERROR_INVALID_DLL);return nullptr;
    }
    SetLastError(load_error);return module;
}
HMODULE load_system_xinput(){return load_fixed_system_library(L"xinput1_4.dll");}
HMODULE load_system_dxgi(){return load_fixed_system_library(L"dxgi.dll");}
HMODULE load_system_d3d11(){return load_fixed_system_library(L"d3d11.dll");}
HMODULE load_system_vulkan(){return load_fixed_system_library(L"vulkan-1.dll");}
NTSTATUS module_file_opened(HANDLE file,const UNICODE_STRING *requested) noexcept {
    if(disabled_modules.empty())return 0;
    PreserveError error;
    bool disabled=requested&&!(requested->Length%sizeof(wchar_t))&&disabled_name(requested->Buffer,requested->Length/sizeof(wchar_t));
    return record_alias(file,disabled);
}
bool module_image_disabled(HANDLE file,ULONG allocation) {
    if(!file||!(allocation&SEC_IMAGE)||(allocation&SEC_IMAGE_NO_EXECUTE)==SEC_IMAGE_NO_EXECUTE
            ||resource_load||system_backend_load||disabled_modules.empty())return false;
    PreserveError error;
    if(alias_generation(file))return true;
    std::vector<wchar_t> path(32768);
    DWORD length=GetFinalPathNameByHandleW(file,path.data(),(DWORD)path.size(),FILE_NAME_OPENED|VOLUME_NAME_DOS);
    if(length&&length<path.size()&&disabled_name(path.data(),length))return true;
    length=GetFinalPathNameByHandleW(file,path.data(),(DWORD)path.size(),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    // An invalid/inaccessible handle retains the underlying NT error. The
    // load-name gate handles direct requests; this gate catches dependencies
    // that Windows binds internally without calling exported LdrLoadDll.
    return length&&length<path.size()&&disabled_name(path.data(),length);
}
void attach_module_runtime(const str &runtime_directory,const std::vector<str> &disabled) {
    disabled_modules=disabled;
    str bundle=runtime_directory+(sizeof(void*)==8?L"\\builtin\\x64\\":L"\\builtin\\x86\\");
    bool any=false;
    size_t index=0;
    for(auto &provider:providers){
        provider.name=str(portable_builtins[index++].name)+L".dll";
        provider.path=bundle+provider.name;
        if(provider.path.size()>32766)throw std::runtime_error("Wine builtin module path is too long");
        HMODULE candidate=GetModuleHandleW(provider.path.c_str());
        if(!candidate)continue;
        // Never infer selection from a basename collision with an app/native DLL.
        std::vector<wchar_t> actual(32768);DWORD count=GetModuleFileNameW(candidate,actual.data(),(DWORD)actual.size());
        if(count&&count<actual.size()&&!_wcsicmp(actual.data(),provider.path.c_str())){provider.module=candidate;any=true;}
    }
    if(!any&&disabled_modules.empty())return; // No selected provider or disabled policy.
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    resolve(real_load,ntdll,"LdrLoadDll");resolve(real_get,ntdll,"LdrGetDllHandle");resolve(real_get_ex,ntdll,"LdrGetDllHandleEx");
    attach(real_load,load);attach(real_get,get);attach(real_get_ex,get_ex);
    attach(real_load_ex_w,load_ex_w);attach(real_load_ex_a,load_ex_a);
    if(!disabled_modules.empty()){
        resolve(real_close_object,ntdll,"NtClose");resolve(real_duplicate_object,ntdll,"NtDuplicateObject");
        resolve(compare_object_handles,GetModuleHandleW(L"kernelbase.dll"),"CompareObjectHandles");
        attach(real_close_object,close_object);attach(real_duplicate_object,duplicate_object);
    }
}
}
