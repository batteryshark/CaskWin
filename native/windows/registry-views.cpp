// SPDX-License-Identifier: LGPL-2.1-or-later
#include "registry-views.h"
#include <unordered_map>
#include <vector>
#include <cwctype>
#include <sddl.h>
#include <stdexcept>
namespace wd {
using text=std::wstring;
namespace {
bool starts(const text &s,const text &prefix){return s.size()>=prefix.size()&&!_wcsnicmp(s.c_str(),prefix.c_str(),prefix.size());}
bool inside(const text &s,const text &prefix){return starts(s,prefix)&&(s.size()==prefix.size()||s[prefix.size()]==L'\\');}
text lower(text value){for(auto &c:value)c=(wchar_t)towlower(c);return value;}
text classes_alias(const text &value) {
    const text users=L"\\REGISTRY\\USER\\";
    if(!starts(value,users))return value;
    size_t end=value.find(L'\\',users.size());if(end==value.npos)return value;
    text base=value.substr(0,end);
    if(inside(value,base+L"\\Software\\Classes"))return base+L"_Classes"+value.substr(end+17);
    return value;
}
text class_without_wow(const text &value) {
    const text machine=L"\\REGISTRY\\MACHINE\\Software";
    if(inside(value,machine+L"\\WOW6432Node\\Classes"))return machine+L"\\Classes"+value.substr(machine.size()+20);
    if(inside(value,machine+L"\\Classes\\WOW6432Node"))return machine+L"\\Classes"+value.substr(machine.size()+20);
    if(starts(value,L"\\REGISTRY\\USER\\")){
        size_t end=value.find(L'\\',15);text root=value.substr(0,end);
        if(root.size()>=8&&!_wcsicmp(root.c_str()+root.size()-8,L"_Classes")&&inside(value,root+L"\\WOW6432Node"))return root+value.substr(root.size()+12);
    }
    return value;
}
[[maybe_unused]] bool has_wow_node(const text &value) {
    text s=lower(value);size_t at=s.find(L"\\wow6432node");
    return at!=s.npos&&(at+12==s.size()||s[at+12]==L'\\');
}
bool routed_namespace(const text &value) {
    if(inside(value,L"\\REGISTRY\\MACHINE\\Software"))return true;
    if(!starts(value,L"\\REGISTRY\\USER\\"))return false;
    size_t end=value.find(L'\\',15);text root=value.substr(0,end);
    return root.size()>=8&&!_wcsicmp(root.c_str()+root.size()-8,L"_Classes");
}
SRWLOCK state_lock=SRWLOCK_INIT;
struct StateLock {
    bool exclusive;
    explicit StateLock(bool write):exclusive(write){if(write)AcquireSRWLockExclusive(&state_lock);else AcquireSRWLockShared(&state_lock);}
    ~StateLock(){if(exclusive)ReleaseSRWLockExclusive(&state_lock);else ReleaseSRWLockShared(&state_lock);}
};
// OS routing is stable within a process. Bound full-name query results so games
// cannot grow this cache without limit by probing many missing registry keys.
// Keep it alive until process teardown; late native opens may outlive the CRT.
auto &routes=*new std::unordered_map<text,text>;
ULONG native_tags(HANDLE key,RegistryQueryKeyFn query) {
    if(!key||!query)return 0;
    ULONG tags=0,length=0;
    // KeyHandleTagsInformation is readable even without KEY_QUERY_VALUE.
    // Unlike a process-local handle map, the key body's tags follow duplicate
    // and inherited handles, including a remote DUPLICATE_CLOSE_SOURCE.
    if(query(key,7,&tags,sizeof(tags),&length)<0)return 0;
    return tags;
}
RegistryView tagged_view(ULONG tags) {
    switch(tags&(KEY_WOW64_32KEY|KEY_WOW64_64KEY)){
    case KEY_WOW64_32KEY:return RegistryView::view32;
    case KEY_WOW64_64KEY:return RegistryView::view64;
    default:return RegistryView::unknown;
    }
}
text native_name(HANDLE key,RegistryQueryKeyFn query) {
    ULONG length=0;query(key,3,nullptr,0,&length);if(length<4||length>65536)return {};
    std::vector<BYTE> bytes(length);if(query(key,3,bytes.data(),length,&length)<0)return {};
    ULONG n=*(ULONG*)bytes.data();return n<=bytes.size()-4?text((wchar_t*)(bytes.data()+4),n/2):text();
}
// Accept only the WOW64 transform, never a general host symbolic-link target.
bool same_or_wow_route(const text &source,const text &target) {
    if(!_wcsicmp(source.c_str(),target.c_str()))return true;
    if(!_wcsicmp(class_without_wow(source).c_str(),class_without_wow(target).c_str()))return true;
    text reduced=target,folded=lower(target);size_t at=folded.find(L"\\wow6432node");
    if(at==folded.npos||(at+12<folded.size()&&folded[at+12]!=L'\\'))return false;
    reduced.erase(at,12);return !_wcsicmp(source.c_str(),reduced.c_str());
}
NTSTATUS open_route_key(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES oa,RegistryOpenKeyFn open) {
#ifdef _WIN64
    // x64 raw NtOpenKey does not implement WOW32 redirection. An inherited
    // x86 app-hive handle also causes KernelBase to skip its normal redirect.
    // Ask KernelBase about the real, read-only native registry instead, using
    // an independently opened root rather than its cached predefined handles
    // (those may already refer to our private hive). The shim calls us guarded.
    text path(oa->ObjectName->Buffer,oa->ObjectName->Length/2);
    const text machine=L"\\REGISTRY\\MACHINE",users=L"\\REGISTRY\\USER";
    text root=inside(path,machine)?machine:inside(path,users)?users:text();
    if(root.empty())return open(handle,access,oa);
    UNICODE_STRING name{(USHORT)(root.size()*2),(USHORT)(root.size()*2),root.data()};
    OBJECT_ATTRIBUTES attr{};attr.Length=sizeof(attr);attr.ObjectName=&name;attr.Attributes=OBJ_CASE_INSENSITIVE;
    HANDLE native=nullptr;NTSTATUS status=open(&native,KEY_READ|KEY_WOW64_64KEY,&attr);if(status<0)return status;
    const wchar_t *tail=path.c_str()+root.size();if(*tail==L'\\')++tail;
    LSTATUS result=RegOpenKeyExW((HKEY)native,tail,0,access,(HKEY*)handle);RegCloseKey((HKEY)native);
    return result==ERROR_SUCCESS?0:result==ERROR_FILE_NOT_FOUND||result==ERROR_PATH_NOT_FOUND?(NTSTATUS)0xc0000034:(NTSTATUS)0xc0000022;
#else
    return open(handle,access,oa);
#endif
}
text query_route(const text &path,ACCESS_MASK view,RegistryOpenKeyFn open,RegistryQueryKeyFn query) {
    text candidate=path;
    // The nearest existing host ancestor identifies the OS's shared/redirected
    // branch even when this private application key has never existed on host.
    for(unsigned depth=0;depth<128&&routed_namespace(candidate);++depth){
        if(candidate.size()>32766)return path;
        UNICODE_STRING name{(USHORT)(candidate.size()*2),(USHORT)(candidate.size()*2),candidate.data()};
        OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;
        HANDLE key=nullptr;NTSTATUS status=open_route_key(&key,KEY_QUERY_VALUE|view,&oa,open);
        if(status>=0){
            text actual;
            try{actual=native_name(key,query);}catch(...){RegCloseKey((HKEY)key);throw;}
            RegCloseKey((HKEY)key);actual=classes_alias(actual);
            if(!actual.empty()&&same_or_wow_route(candidate,actual))return actual+path.substr(candidate.size());
            return path;
        }
        size_t slash=candidate.find_last_of(L'\\');if(slash==candidate.npos)break;candidate.resize(slash);
    }
    return path;
}
text route(const text &path,ACCESS_MASK view,RegistryOpenKeyFn open,RegistryQueryKeyFn query) {
    text key=std::to_wstring(view)+L"|"+lower(path),result;bool found=false;
    {StateLock lock(false);auto it=routes.find(key);if(it!=routes.end()){result=it->second;found=true;}}
    if(found)return result;
    result=query_route(path,view,open,query);
    {StateLock lock(true);if(routes.size()>=256)routes.clear();routes.emplace(std::move(key),result);}return result;
}
text current_user_sid() {
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Cannot read registry import user token");
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);
    BOOL ok=GetTokenInformation(token,TokenUser,data.data(),size,&size);CloseHandle(token);if(!ok)throw std::runtime_error("Cannot read registry import user SID");
    LPWSTR value=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)data.data())->User.Sid,&value))throw std::runtime_error("Cannot format registry import user SID");
    text result=value;LocalFree(value);return result;
}
NTSTATUS NTAPI open_import_route(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES oa) {
    text path(oa->ObjectName->Buffer,oa->ObjectName->Length/2),tail;HKEY root=nullptr;
    const text machine=L"\\REGISTRY\\MACHINE",users=L"\\REGISTRY\\USER";
    if(inside(path,machine)){root=HKEY_LOCAL_MACHINE;tail=path.substr(machine.size());}
    else if(inside(path,users)){root=HKEY_USERS;tail=path.substr(users.size());}
    else return (NTSTATUS)0xc0000034;
    if(!tail.empty()&&tail[0]==L'\\')tail.erase(0,1);
    // KernelBase, unlike raw x64 NtOpenKey, implements KEY_WOW64_32KEY.
    LSTATUS status=RegOpenKeyExW(root,tail.c_str(),0,access,(HKEY*)handle);
    return status==ERROR_SUCCESS?0:status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND?(NTSTATUS)0xc0000034:(NTSTATUS)0xc0000022;
}
}
text registry_import_name(const text &private_name,REGSAM view) {
    if(view!=KEY_WOW64_32KEY&&view!=KEY_WOW64_64KEY)throw std::runtime_error("Expected /reg:32 or /reg:64");
    const text machine=L"\\REGISTRY\\MACHINE";
    text user,logical;
    if(inside(private_name,L"Machine"))logical=machine+private_name.substr(7);
    else if(inside(private_name,L"User")){user=L"\\REGISTRY\\USER\\"+current_user_sid();logical=classes_alias(user+private_name.substr(4));}
    else return private_name; // .Default and physical Wine roots stay unchanged.
    auto query=(RegistryQueryKeyFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryKey");
    if(!query)throw std::runtime_error("NtQueryKey is unavailable for registry import routing");
    if(view==KEY_WOW64_32KEY||class_without_wow(logical)!=logical)logical=route(logical,view,open_import_route,query);
    if(inside(logical,machine))return L"Machine"+logical.substr(machine.size());
    if(!user.empty()&&inside(logical,user+L"_Classes"))return L"User\\Software\\Classes"+logical.substr(user.size()+8);
    if(!user.empty()&&inside(logical,user))return L"User"+logical.substr(user.size());
    throw std::runtime_error("Registry import view escaped its private root");
}
ACCESS_MASK registry_view_access(ACCESS_MASK access,RegistryView view) {
    if(access&(KEY_WOW64_32KEY|KEY_WOW64_64KEY))return access;
    return access|(view==RegistryView::view32?KEY_WOW64_32KEY:view==RegistryView::view64?KEY_WOW64_64KEY:0);
}
RegistryViewRoute registry_view_name(const text &logical,ACCESS_MASK access,HANDLE parent,const text &parent_name,RegistryOpenKeyFn open,RegistryQueryKeyFn query) {
    ULONG tags=parent?native_tags(parent,query):0;
    RegistryView view=RegistryView::unknown;
    if(access&KEY_WOW64_64KEY)view=RegistryView::view64;
    else if(access&KEY_WOW64_32KEY)view=RegistryView::view32;
    else if(parent)view=tagged_view(tags);
    text path=classes_alias(logical),base=classes_alias(parent_name);
    if(view==RegistryView::unknown){
#ifdef _WIN64
        view=RegistryView::view64;
#else
        // An inherited canonical Software handle without a Wow node already
        // names the native view. Root Machine/User handles select process view.
        view=parent&&!base.empty()&&inside(base,L"\\REGISTRY\\MACHINE\\Software")&&!has_wow_node(base)?RegistryView::view64:RegistryView::view32;
#endif
    }
    // Native HKCU Software -> Classes is a link transition. With no explicit
    // child flag it drops an inherited 64-bit tag; later opens use the caller's
    // default view. Do not make that tag sticky merely because our private
    // hive represents the same link with an ordinary directory.
    if(view==RegistryView::view64&&!(access&(KEY_WOW64_32KEY|KEY_WOW64_64KEY))&&starts(base,L"\\REGISTRY\\USER\\")){
        size_t end=base.find(L'\\',15);
        if(end!=base.npos&&!_wcsicmp(base.c_str()+end,L"\\Software")&&!_wcsicmp(path.c_str(),(base.substr(0,end)+L"_Classes").c_str()))return {path,RegistryView::unknown};
    }
#ifdef _WIN64
    // Keep raw x64 NT semantics for ordinary calls: KernelBase already inserted
    // their physical WOW path. Only incoming app-hive handles suppress that
    // native Win32 step (tag 0x400), so they need the additional view routing.
    if(class_without_wow(path)!=path&&open&&query)return {route(path,KEY_WOW64_64KEY,open,query),view};
    if(!(tags&0x400))return {path,view};
#endif
    if(class_without_wow(path)!=path&&open&&query)return {route(path,view==RegistryView::view64?KEY_WOW64_64KEY:KEY_WOW64_32KEY,open,query),view};
    if(has_wow_node(path))return {path,view};
    if(view==RegistryView::view64||!routed_namespace(path)||!open||!query)return {path,view};
    return {route(path,KEY_WOW64_32KEY,open,query),view};
}
}
