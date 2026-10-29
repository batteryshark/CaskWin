// SPDX-License-Identifier: LGPL-2.1-or-later
#include "named-objects.h"
#include "detours.h"
#include <cstring>

namespace wd {
namespace {
str object_prefix,system32,syswow64;
HMODULE this_module,kernel32_module,kernelbase_module,ntdll_module;
bool (*enabled_callback)();
thread_local unsigned recursion;
struct Guard {Guard(){++recursion;}~Guard(){--recursion;}};
using QueryObject=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
QueryObject query_object;
using Open=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
using Mutant=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,BOOLEAN);
using Event=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,BOOLEAN);
using Semaphore=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,LONG,LONG);
using Timer=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG);
Open real_open_mutant,real_open_event,real_open_semaphore,real_open_timer,real_open_section;
Mutant real_create_mutant;Event real_create_event;Semaphore real_create_semaphore;Timer real_create_timer;
constexpr NTSTATUS no_memory=(NTSTATUS)0xc0000017,too_long=(NTSTATUS)0xc0000106;
bool under(const str &path,const str &directory){return path.size()>directory.size()&&path[directory.size()]==L'\\'&&!_wcsnicmp(path.c_str(),directory.c_str(),directory.size());}
bool application_origin(){
    // NT entry points normally see KernelBase as their immediate caller. Walk
    // through only the API wrappers and this shim; the first real consumer
    // decides whether this is application IPC or Windows backend IPC. In
    // particular, CLR/CoreCLR synchronization belongs to the application.
    void *frames[32]{};USHORT count=CaptureStackBackTrace(0,32,frames,nullptr);
    for(USHORT i=0;i<count;i++){
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)frames[i],&module))continue;
        if(module==this_module||module==kernel32_module||module==kernelbase_module||module==ntdll_module)continue;
        std::vector<wchar_t> buffer(32768);DWORD n=GetModuleFileNameW(module,buffer.data(),32768);
        if(!n||n>=32768)return false;str path(buffer.data(),n);auto slash=path.find_last_of(L"\\/");const wchar_t *base=path.c_str()+(slash==str::npos?0:slash+1);
        if(!_wcsicmp(base,L"clr.dll")||!_wcsicmp(base,L"coreclr.dll")||!_wcsicmp(base,L"mscorwks.dll"))return true;
        if(!_wcsicmp(base,L"mscoree.dll"))continue;
        return !under(path,system32)&&!under(path,syswow64);
    }
    return false;
}
str directory_name(HANDLE handle){
    alignas(void*) BYTE small[1024]{};ULONG needed=0;
    NTSTATUS status=query_object(handle,1,small,sizeof(small),&needed);
    std::vector<BYTE> large;BYTE *data=small;size_t size=sizeof(small);
    if(status<0){if(!needed||needed>65536)return {};large.resize(needed);data=large.data();size=large.size();status=query_object(handle,1,data,(ULONG)size,&needed);}
    if(status<0||size<sizeof(UNICODE_STRING))return {};
    auto name=(UNICODE_STRING*)data;uintptr_t at=(uintptr_t)name->Buffer,start=(uintptr_t)data;
    if(name->Length%2||at<start||at-start>size||name->Length>size-(at-start))return {};
    return str(name->Buffer,name->Length/2);
}
bool base_directory(const str &path){
    if(!_wcsicmp(path.c_str(),L"\\BaseNamedObjects"))return true;
    constexpr wchar_t sessions[]=L"\\Sessions\\";constexpr size_t length=sizeof(sessions)/sizeof(*sessions)-1;
    if(path.size()<=length||_wcsnicmp(path.c_str(),sessions,length))return false;
    size_t end=length;while(end<path.size()&&path[end]>=L'0'&&path[end]<=L'9')++end;
    return end>length&&!_wcsicmp(path.c_str()+end,L"\\BaseNamedObjects");
}
bool routing_enabled(){return !recursion&&enabled_callback&&enabled_callback();}
template<class F>NTSTATUS routed(POBJECT_ATTRIBUTES attributes,F call){
    if(!routing_enabled()||!attributes||!attributes->ObjectName||!attributes->ObjectName->Length)return call(attributes);
    Guard guard;
    try{NamedObjectAttributes name(attributes);return call(name.get());}
    catch(const std::length_error&){return too_long;}catch(...){return no_memory;}
}
NTSTATUS NTAPI create_mutant(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,BOOLEAN owner){return routed(a,[&](auto name){return real_create_mutant(h,access,name,owner);});}
NTSTATUS NTAPI create_event(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,ULONG type,BOOLEAN state){return routed(a,[&](auto name){return real_create_event(h,access,name,type,state);});}
NTSTATUS NTAPI create_semaphore(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,LONG initial,LONG maximum){return routed(a,[&](auto name){return real_create_semaphore(h,access,name,initial,maximum);});}
NTSTATUS NTAPI create_timer(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,ULONG type){return routed(a,[&](auto name){return real_create_timer(h,access,name,type);});}
#define OPEN_HOOK(kind) NTSTATUS NTAPI open_##kind(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a){return routed(a,[&](auto name){return real_open_##kind(h,access,name);});}
OPEN_HOOK(mutant) OPEN_HOOK(event) OPEN_HOOK(semaphore) OPEN_HOOK(timer) OPEN_HOOK(section)
#undef OPEN_HOOK
template<class T,class F>void attach(T &real,const char *name,F hook){
    real=(T)GetProcAddress(ntdll_module,name);if(!real)throw std::runtime_error(std::string("Missing named object API: ")+name);
    LONG status=DetourAttach((PVOID*)&real,(PVOID)hook);if(status)throw std::runtime_error(std::string("Cannot attach named object API: ")+name);
}
}
NamedObjectAttributes::NamedObjectAttributes(POBJECT_ATTRIBUTES original):selected(original){
    if(object_prefix.empty()||!original||original->Length!=sizeof(*original)||!original->ObjectName||
       !original->ObjectName->Buffer||!original->ObjectName->Length||original->ObjectName->Length%2||
       original->ObjectName->Length>original->ObjectName->MaximumLength)return;
    const auto &raw=*original->ObjectName;str value(raw.Buffer,raw.Length/2);
    if(value.find(L'\0')!=str::npos)return;
    str directory;size_t leaf=0;
    if(original->RootDirectory){
        if(value[0]==L'\\')return;directory=directory_name(original->RootDirectory);
        auto slash=value.find_last_of(L'\\');
        if(slash!=str::npos){str qualifier=value.substr(0,slash);if(qualifier!=L"Local"&&qualifier!=L"Global")return;leaf=slash+1;}
    }else{
        auto slash=value.find_last_of(L'\\');if(slash==str::npos)return;directory=value.substr(0,slash);leaf=slash+1;
        auto alias=directory.find_last_of(L'\\');if(alias!=str::npos&&(directory.substr(alias+1)==L"Local"||directory.substr(alias+1)==L"Global"))directory.resize(alias);
    }
    if(!base_directory(directory)||leaf==value.size()||value.compare(leaf,object_prefix.size(),object_prefix)==0||!application_origin())return;
    if(value.size()+object_prefix.size()>32766)throw std::length_error("Named object name too long");
    storage=value.substr(0,leaf)+object_prefix+value.substr(leaf);
    name.Buffer=storage.data();name.Length=(USHORT)(storage.size()*sizeof(wchar_t));name.MaximumLength=name.Length+sizeof(wchar_t);
    attributes=*original;attributes.ObjectName=&name;selected=&attributes;
}
void attach_named_objects(const str &identity,bool (*enabled)()){
    constexpr wchar_t media[]=L"Local\\Winedows.Media.";constexpr size_t length=sizeof(media)/sizeof(*media)-1;
    if(identity.size()!=length+32||identity.compare(0,length,media))throw std::runtime_error("Invalid named object session identity");
    for(size_t i=length;i<identity.size();i++)if(!((identity[i]>=L'0'&&identity[i]<=L'9')||(identity[i]>=L'a'&&identity[i]<=L'f')))throw std::runtime_error("Invalid named object session token");
    object_prefix=L"Winedows.Objects."+identity.substr(length)+L".";enabled_callback=enabled;
    std::vector<wchar_t> windows(32768);UINT n=GetWindowsDirectoryW(windows.data(),(UINT)windows.size());if(!n||n>=windows.size())throw std::runtime_error("Cannot identify native object backends");
    system32=str(windows.data(),n)+L"\\System32";syswow64=str(windows.data(),n)+L"\\SysWOW64";
    kernel32_module=GetModuleHandleW(L"kernel32.dll");kernelbase_module=GetModuleHandleW(L"kernelbase.dll");ntdll_module=GetModuleHandleW(L"ntdll.dll");
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&attach_named_objects,&this_module);
    query_object=(QueryObject)GetProcAddress(ntdll_module,"NtQueryObject");if(!query_object)throw std::runtime_error("NtQueryObject unavailable");
    attach(real_create_mutant,"NtCreateMutant",create_mutant);attach(real_open_mutant,"NtOpenMutant",open_mutant);
    attach(real_create_event,"NtCreateEvent",create_event);attach(real_open_event,"NtOpenEvent",open_event);
    attach(real_create_semaphore,"NtCreateSemaphore",create_semaphore);attach(real_open_semaphore,"NtOpenSemaphore",open_semaphore);
    attach(real_create_timer,"NtCreateTimer",create_timer);attach(real_open_timer,"NtOpenTimer",open_timer);
    attach(real_open_section,"NtOpenSection",open_section);
}
}
