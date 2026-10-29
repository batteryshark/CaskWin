// SPDX-License-Identifier: LGPL-2.1-or-later
// Private registry/COM conformance fixture. Host mode is strictly read-only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include <sddl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include "registry-views.h"

static const IID fixture_iid={0xa93a8cec,0x41f6,0x4ef0,{0x99,0x94,0x7e,0x7a,0xe4,0xc4,0xb1,0x06}};
struct RegistryFixture: IUnknown {virtual HRESULT STDMETHODCALLTYPE Identity(DWORD *bits,wchar_t *path,DWORD capacity)=0;};

#ifdef WINEDOWS_REGISTRY_COM_DLL
static HMODULE self;
static LONG objects,locks;
class Fixture final:public RegistryFixture {
    LONG refs=1;
public:
    Fixture(){InterlockedIncrement(&objects);}
    ~Fixture(){InterlockedDecrement(&objects);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid!=IID_IUnknown&&iid!=fixture_iid)return E_NOINTERFACE;
        *out=static_cast<RegistryFixture*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override {LONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Identity(DWORD *bits,wchar_t *path,DWORD capacity) override {
        if(!bits||!path||!capacity)return E_POINTER;*bits=sizeof(void*)*8;
        DWORD n=GetModuleFileNameW(self,path,capacity);return n&&n<capacity?S_OK:E_FAIL;
    }
};
class Factory final:public IClassFactory {
    LONG refs=1;
public:
    Factory(){InterlockedIncrement(&objects);}
    ~Factory(){InterlockedDecrement(&objects);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid!=IID_IUnknown&&iid!=IID_IClassFactory)return E_NOINTERFACE;
        *out=static_cast<IClassFactory*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override {LONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer,REFIID iid,void **out) override {
        if(outer)return CLASS_E_NOAGGREGATION;auto p=new Fixture;HRESULT hr=p->QueryInterface(iid,out);p->Release();return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {if(lock)InterlockedIncrement(&locks);else InterlockedDecrement(&locks);return S_OK;}
};
// This test-only server accepts the unique CLSID generated for each suite.
extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID,REFIID iid,void **out) {
    auto p=new Factory;HRESULT hr=p->QueryInterface(iid,out);p->Release();return hr;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(){return !objects&&!locks?S_OK:S_FALSE;}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;}
#else
static int failures;
static void check(bool ok,const char *name,ULONG status=0) {
    printf("%s %s (status=%08lx)\n",ok?"PASS":"FAIL",name,status);if(!ok)++failures;
}
using QueryKey=NTSTATUS(NTAPI*)(HANDLE,int,void*,ULONG,ULONG*);
using OpenKey=NTSTATUS(NTAPI*)(HANDLE*,ACCESS_MASK,OBJECT_ATTRIBUTES*);
static QueryKey query_key;
static OpenKey open_key;
static std::wstring sid() {
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return {};
    DWORD needed=0;GetTokenInformation(token,TokenUser,nullptr,0,&needed);std::vector<BYTE> bytes(needed);
    bool ok=GetTokenInformation(token,TokenUser,bytes.data(),needed,&needed)!=FALSE;CloseHandle(token);if(!ok)return {};
    LPWSTR text=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)bytes.data())->User.Sid,&text))return {};
    std::wstring result=text;LocalFree(text);return result;
}
static HKEY create(HKEY root,const std::wstring &path,REGSAM view=0) {
    HKEY key=nullptr;LSTATUS s=RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS|view,nullptr,&key,nullptr);
    check(!s,"create private registry key",s);return key;
}
static void set(HKEY key,const wchar_t *name,const std::wstring &value) {
    LSTATUS s=key?RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value.c_str(),(DWORD)(value.size()+1)*2):ERROR_INVALID_HANDLE;
    check(!s,"write private registry value",s);
}
static void expect(HKEY root,const std::wstring &path,const wchar_t *name,const std::wstring &value,const char *label,REGSAM view=0) {
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,path.c_str(),0,KEY_READ|view,&key);wchar_t data[1024]{};DWORD size=sizeof(data),type=0;
    if(!s){s=RegQueryValueExW(key,name,nullptr,&type,(BYTE*)data,&size);RegCloseKey(key);}
    check(!s&&type==REG_SZ&&size>=2&&size<=sizeof(data)&&data[size/2-1]==0&&value==data,label,s);
}
static void absent(HKEY root,const std::wstring &path,REGSAM view=0) {
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,path.c_str(),0,KEY_READ|view,&key);if(key)RegCloseKey(key);
    if(s!=ERROR_FILE_NOT_FOUND&&s!=ERROR_PATH_NOT_FOUND)wprintf(L"Unexpected key: %ls\n",path.c_str());
    check(s==ERROR_FILE_NOT_FOUND||s==ERROR_PATH_NOT_FOUND,"unique test key absent",s);
}
static void name_contract(HKEY key,const std::wstring &expected) {
    const ULONG required=4+(ULONG)expected.size()*2;
    ULONG returned=0;NTSTATUS s=query_key(key,3,nullptr,0,&returned);
    check(s==(NTSTATUS)0xc0000023&&returned==required,"NtQueryKey zero buffer reports logical byte length",s);
    for(ULONG capacity:std::vector<ULONG>{1,3,4,6,required-1,required,required+8}){
        std::vector<BYTE> bytes(required+32,0xcc);returned=0;
        s=query_key(key,3,bytes.data(),capacity,&returned);
        NTSTATUS wanted=capacity<4?(NTSTATUS)0xc0000023:capacity<required?(NTSTATUS)0x80000005:0;
        check(s==wanted&&returned==required,"NtQueryKey exact short/full status and length",s);
        // Windows may expose intermediate data on overflow; only successful
        // queries promise usable information in the caller's buffer.
        if(!s){ULONG name_bytes=0;memcpy(&name_bytes,bytes.data(),4);check(name_bytes==required-4,"NtQueryKey NameLength excludes header and terminator");}
        if(capacity>=required){std::wstring actual((wchar_t*)(bytes.data()+4),expected.size());if(_wcsicmp(actual.c_str(),expected.c_str()))wprintf(L"Actual NT name: %ls; expected: %ls\n",actual.c_str(),expected.c_str());check(!_wcsicmp(actual.c_str(),expected.c_str()),"NtQueryKey returns logical key name");}
        ULONG untouched=capacity<4?0:capacity;
        check(std::all_of(bytes.begin()+untouched,bytes.end(),[](BYTE b){return b==0xcc;}),"NtQueryKey never writes outside caller capacity");
    }
}
static HKEY nt_open(const std::wstring &path,HKEY parent=nullptr) {
    UNICODE_STRING name{(USHORT)(path.size()*2),(USHORT)(path.size()*2),const_cast<wchar_t*>(path.data())};
    OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.RootDirectory=parent;oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;HANDLE key=nullptr;
    NTSTATUS s=open_key(&key,KEY_READ|KEY_WOW64_64KEY,&oa);check(!s,"NtOpenKey logical absolute/relative path",s);return (HKEY)key;
}
static std::wstring classes(const std::wstring &token){return L"Winedows.Registry."+token;}
static void names_and_classes(const std::wstring &token) {
    std::wstring user_sid=sid();check(!user_sid.empty(),"read current user SID");
    std::wstring path=L"Software\\WinedowsRegistryTests\\"+token+L"\\"+std::to_wstring(sizeof(void*)*8);
    for(HKEY root:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}){
        std::wstring logical=root==HKEY_LOCAL_MACHINE?L"\\REGISTRY\\MACHINE\\":L"\\REGISTRY\\USER\\"+user_sid+L"\\";
        HKEY key=create(root,path,KEY_WOW64_64KEY);if(!key)continue;set(key,L"Marker",token);name_contract(key,logical+path);
        HKEY child=create(key,L"Child");if(child){name_contract(child,logical+path+L"\\Child");RegCloseKey(child);}
        HKEY reopened=nt_open(logical+path);if(reopened){expect(reopened,L"",L"Marker",token,"logical NT name reopens original private key");RegCloseKey(reopened);}
        reopened=nt_open(L"Child",key);if(reopened){name_contract(reopened,logical+path+L"\\Child");RegCloseKey(reopened);}RegCloseKey(key);
    }
    const std::wstring base=classes(token);
    for(const wchar_t *suffix:{L".MachineOnly",L".Overlap"}){HKEY k=create(HKEY_LOCAL_MACHINE,L"Software\\Classes\\"+base+suffix);if(k){set(k,nullptr,L"machine");RegCloseKey(k);}}
    for(const wchar_t *suffix:{L".UserOnly",L".Overlap"}){HKEY k=create(HKEY_CURRENT_USER,L"Software\\Classes\\"+base+suffix);if(k){set(k,nullptr,L"user");RegCloseKey(k);}}
    expect(HKEY_CLASSES_ROOT,base+L".MachineOnly",nullptr,L"machine","HKCR includes machine-only class");
    expect(HKEY_CLASSES_ROOT,base+L".UserOnly",nullptr,L"user","HKCR includes user-only class");
    expect(HKEY_CLASSES_ROOT,base+L".Overlap",nullptr,L"user","HKCR duplicate class prefers user");
    HKEY k=create(HKEY_CLASSES_ROOT,base+L".Created");if(k){set(k,nullptr,L"created");RegCloseKey(k);}
    expect(HKEY_LOCAL_MACHINE,L"Software\\Classes\\"+base+L".Created",nullptr,L"created","new HKCR class stored in private machine classes");
    k=create(HKEY_CLASSES_ROOT,base+L".Overlap");if(k){set(k,nullptr,L"updated-user");RegCloseKey(k);}
    expect(HKEY_CURRENT_USER,L"Software\\Classes\\"+base+L".Overlap",nullptr,L"updated-user","HKCR write to existing user class stays user");
    expect(HKEY_LOCAL_MACHINE,L"Software\\Classes\\"+base+L".Overlap",nullptr,L"machine","HKCR user write preserves machine class");
    k=nt_open(L"\\REGISTRY\\USER\\"+user_sid+L"_Classes\\"+base+L".UserOnly");
    if(k){expect(k,L"",nullptr,L"user","NT user _Classes alias maps to private user classes");name_contract(k,L"\\REGISTRY\\USER\\"+user_sid+L"_Classes\\"+base+L".UserOnly");RegCloseKey(k);}
}
static std::wstring server_name(bool user){return L"C:\\tests\\registry-"+std::wstring(user?L"user":L"machine")+std::to_wstring(sizeof(void*)*8)+L".dll";}
static void register_server(const std::wstring &clsid,bool user) {
    HKEY k=create(user?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE,L"Software\\Classes\\CLSID\\"+clsid+L"\\InprocServer32");
    if(k){set(k,nullptr,server_name(user));set(k,L"ThreadingModel",L"Both");RegCloseKey(k);}
    expect(HKEY_CLASSES_ROOT,L"CLSID\\"+clsid+L"\\InprocServer32",nullptr,server_name(user),"private COM registration visible through HKCR");
}
static void activate(const std::wstring &text,bool user) {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);check(SUCCEEDED(hr),"CoInitializeEx",hr);if(FAILED(hr))return;
    CLSID clsid{};hr=CLSIDFromString(text.c_str(),&clsid);check(SUCCEEDED(hr),"parse unique fixture CLSID",hr);
    RegistryFixture *object=nullptr;if(SUCCEEDED(hr))hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,fixture_iid,(void**)&object);
    check(SUCCEEDED(hr)&&object,"CoCreateInstance uses private registry registration",hr);
    if(object){DWORD bits=0;wchar_t path[32768]{};hr=object->Identity(&bits,path,32768);std::wstring expected=server_name(user);
        check(SUCCEEDED(hr)&&bits==sizeof(void*)*8,"private COM server interface and architecture",hr);
        wprintf(L"Activated server: %ls\n",path);
        check(!_wcsicmp(path,expected.c_str()),"COM selected exact private machine/user DLL path");object->Release();}
    CoFreeUnusedLibraries();CoUninitialize();
}
static void audit_absent(const std::wstring &token,const std::wstring &clsid) {
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        for(HKEY root:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}){
            absent(root,L"Software\\WinedowsRegistryTests\\"+token,view);
            for(const wchar_t *suffix:{L".MachineOnly",L".UserOnly",L".Overlap",L".Created",L".ImportShared",L".ImportUserShared"})absent(root,L"Software\\Classes\\"+classes(token)+suffix,view);
            absent(root,L"Software\\Classes\\CLSID\\"+clsid,view);
        }
        absent(HKEY_CLASSES_ROOT,L"CLSID\\"+clsid,view);
    }
}
static void host_names() {
    std::wstring user=L"\\REGISTRY\\USER\\"+sid();
    for(HKEY root:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}){
        HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,L"Software",0,KEY_READ|KEY_WOW64_64KEY,&key);
        check(!s,"open existing host Software key read-only",s);
        if(key){name_contract(key,(root==HKEY_LOCAL_MACHINE?L"\\REGISTRY\\MACHINE":user)+L"\\Software");RegCloseKey(key);}
    }
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Classes",0,KEY_READ,&key);
    check(!s,"open existing host user classes key read-only",s);
    if(key){ULONG required=0;query_key(key,3,nullptr,0,&required);std::vector<BYTE> bytes(required+2,0);NTSTATUS status=query_key(key,3,bytes.data(),required,&required);
        if(!status)wprintf(L"Native HKCU Software Classes name: %ls\n",(wchar_t*)(bytes.data()+4));
        name_contract(key,user+L"_Classes");RegCloseKey(key);}
}
static void wow64_views(const std::wstring &token) {
    std::wstring path=L"Software\\WinedowsRegistryTests\\"+token+L"\\Views";
    HKEY key=create(HKEY_LOCAL_MACHINE,path,KEY_WOW64_32KEY);if(key){set(key,L"View",L"32");RegCloseKey(key);}
    key=create(HKEY_LOCAL_MACHINE,path,KEY_WOW64_64KEY);if(key){set(key,L"View",L"64");RegCloseKey(key);}
    expect(HKEY_LOCAL_MACHINE,path,L"View",L"32","WOW64 32-bit HKLM view stays independent",KEY_WOW64_32KEY);
    expect(HKEY_LOCAL_MACHINE,path,L"View",L"64","WOW64 64-bit HKLM view stays independent",KEY_WOW64_64KEY);
    expect(HKEY_LOCAL_MACHINE,path,L"View",sizeof(void*)==4?L"32":L"64","default HKLM view follows process architecture");
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY parent=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_ALL_ACCESS|view,&parent);check(!s,"open explicit-view parent",s);if(s)continue;
        key=create(parent,L"InheritedChild");if(key){set(key,L"View",view==KEY_WOW64_32KEY?L"32":L"64");RegCloseKey(key);}RegCloseKey(parent);
    }
    expect(HKEY_LOCAL_MACHINE,path+L"\\InheritedChild",L"View",L"32","relative default child retains 32-bit parent view",KEY_WOW64_32KEY);
    expect(HKEY_LOCAL_MACHINE,path+L"\\InheritedChild",L"View",L"64","relative default child retains 64-bit parent view",KEY_WOW64_64KEY);
}
static void print_name(HKEY key,const wchar_t *api,const std::wstring &requested,REGSAM view,LSTATUS status) {
    std::wstring actual;
    if(key){ULONG length=0;query_key(key,3,nullptr,0,&length);if(length>=4&&length<65536){std::vector<BYTE> bytes(length);if(!query_key(key,3,bytes.data(),length,&length)){ULONG n=*(ULONG*)bytes.data();if(n<=bytes.size()-4)actual.assign((wchar_t*)(bytes.data()+4),n/2);}}}
    wprintf(L"VIEW %ls flags=%04lx status=%08lx request=%ls actual=%ls\n",api,view,(ULONG)status,requested.c_str(),actual.c_str());
}
static void host_views() {
    const std::wstring user=L"\\REGISTRY\\USER\\"+sid();
    for(HKEY root:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER})for(const wchar_t *path:{L"Software",L"Software\\Classes",L"Software\\Classes\\CLSID",L"Software\\Classes\\Interface",L"Software\\Classes\\AppID",L"Software\\Microsoft\\Windows\\CurrentVersion",L"Software\\Policies"})for(REGSAM view:std::vector<REGSAM>{0,KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(root,path,0,KEY_READ|view,&key);print_name(key,L"RegOpen",std::wstring(root==HKEY_LOCAL_MACHINE?L"HKLM\\":L"HKCU\\")+path,view,s);if(key)RegCloseKey(key);
        std::wstring full=(root==HKEY_LOCAL_MACHINE?L"\\REGISTRY\\MACHINE":user)+L"\\"+path;
        UNICODE_STRING name{(USHORT)(full.size()*2),(USHORT)(full.size()*2),full.data()};OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;
        key=nullptr;NTSTATUS ns=open_key((HANDLE*)&key,KEY_QUERY_VALUE|view,&oa);print_name(key,L"NtOpen(metadata)",full,view,ns);if(key)RegCloseKey(key);
    }
    for(const wchar_t *parent_path:{L"Software",L"Software\\Classes"})for(REGSAM parent_view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY parent=nullptr;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,parent_path,0,KEY_READ|parent_view,&parent))continue;
        for(REGSAM view:std::vector<REGSAM>{0,KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
            std::wstring tail=!wcscmp(parent_path,L"Software")?L"Microsoft\\Windows\\CurrentVersion":L"CLSID";std::wstring label=std::wstring(parent_path)+L"("+std::to_wstring(parent_view)+L")\\"+tail;
            HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(parent,tail.c_str(),0,KEY_READ|view,&key);print_name(key,L"RegRelative",label,view,s);if(key)RegCloseKey(key);
            UNICODE_STRING name{(USHORT)(tail.size()*2),(USHORT)(tail.size()*2),tail.data()};OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.RootDirectory=parent;oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;
            key=nullptr;NTSTATUS ns=open_key((HANDLE*)&key,KEY_READ|view,&oa);print_name(key,L"NtRelative",label,view,ns);if(key)RegCloseKey(key);
        }RegCloseKey(parent);
    }
}
static std::wstring queried_name(HKEY key) {
    ULONG size=0;query_key(key,3,nullptr,0,&size);if(size<4||size>65536)return {};
    std::vector<BYTE> bytes(size);if(query_key(key,3,bytes.data(),size,&size)<0)return {};
    ULONG n=*(ULONG*)bytes.data();return n<=bytes.size()-4?std::wstring((wchar_t*)(bytes.data()+4),n/2):std::wstring();
}
static unsigned route_open_count;
static NTSTATUS NTAPI counted_route_open(HANDLE *key,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa){++route_open_count;return open_key(key,access,oa);}
static void host_route_tests() {
    const std::wstring machine=L"\\REGISTRY\\MACHINE",user=L"\\REGISTRY\\USER\\"+sid();
    for(const auto &root:{machine,user})for(const wchar_t *tail:{L"\\Software",L"\\Software\\Classes",L"\\Software\\Classes\\CLSID",L"\\Software\\Classes\\Interface",L"\\Software\\Classes\\AppID",L"\\Software\\Policies",L"\\Software\\WOW6432Node\\Classes",L"\\Software\\WOW6432Node\\Classes\\CLSID",L"\\Software\\WOW6432Node\\Classes\\AppID"})for(REGSAM view:std::vector<REGSAM>{0,KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        std::wstring path=root+tail;UNICODE_STRING name{(USHORT)(path.size()*2),(USHORT)(path.size()*2),path.data()};OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;
        HKEY key=nullptr;NTSTATUS s=open_key((HANDLE*)&key,KEY_QUERY_VALUE|view,&oa);if(s<0){printf("SKIP absent native route reference (status=%08lx)\n",(ULONG)s);continue;}
        std::wstring native=queried_name(key);RegCloseKey(key);
        auto actual=wd::registry_view_name(path,view,nullptr,L"",open_key,query_key);
        if(_wcsicmp(actual.name.c_str(),native.c_str()))wprintf(L"Route mismatch: request=%ls flags=%04lx actual=%ls native=%ls\n",path.c_str(),view,actual.name.c_str(),native.c_str());
        check(!_wcsicmp(actual.name.c_str(),native.c_str()),"helper absolute route matches native NT path");
    }
    for(const wchar_t *parent_path:{L"Software",L"Software\\Classes"})for(REGSAM parent_view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY parent=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,parent_path,0,KEY_READ|parent_view,&parent);check(!s,"open native route-test parent read-only",s);if(s)continue;
        std::wstring base=queried_name(parent);
        std::vector<std::wstring> tails=!wcscmp(parent_path,L"Software")?std::vector<std::wstring>{L"Microsoft\\Windows\\CurrentVersion",L"Classes\\CLSID",L"Classes\\AppID"}:std::vector<std::wstring>{L"CLSID"};
        for(auto &tail:tails){
        for(REGSAM view:std::vector<REGSAM>{0,KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
            UNICODE_STRING name{(USHORT)(tail.size()*2),(USHORT)(tail.size()*2),tail.data()};OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.RootDirectory=parent;oa.ObjectName=&name;oa.Attributes=OBJ_CASE_INSENSITIVE;
            HKEY key=nullptr;NTSTATUS ns=open_key((HANDLE*)&key,KEY_QUERY_VALUE|view,&oa);check(!ns,"open native relative route reference",ns);if(ns<0)continue;
            std::wstring native=queried_name(key);RegCloseKey(key);auto actual=wd::registry_view_name(base+L"\\"+tail,view,parent,base,open_key,query_key);
            if(_wcsicmp(actual.name.c_str(),native.c_str()))wprintf(L"Relative route mismatch: parent=%ls flags=%04lx actual=%ls native=%ls\n",base.c_str(),view,actual.name.c_str(),native.c_str());
            check(!_wcsicmp(actual.name.c_str(),native.c_str()),"helper relative route and inherited view match native NT");
        }
        }
        RegCloseKey(parent);
    }
    const std::wstring tail=L"\\Winedows.RouteFixture."+std::to_wstring(GetCurrentProcessId())+L"\\AbsentChild";
    const std::wstring path=machine+L"\\Software"+tail;
    const std::wstring expected=machine+L"\\Software"+(sizeof(void*)==4?L"\\WOW6432Node":L"")+tail;
    auto first=wd::registry_view_name(path,KEY_WOW64_32KEY,nullptr,L"",counted_route_open,query_key);
    check(!_wcsicmp(first.name.c_str(),expected.c_str()),"unregistered private key follows native ancestor routing");
    unsigned count=route_open_count;auto again=wd::registry_view_name(path,KEY_WOW64_32KEY,nullptr,L"",counted_route_open,query_key);
    check(!_wcsicmp(again.name.c_str(),expected.c_str())&&route_open_count==count,"repeated route avoids additional host metadata queries");
}
static void host_import_routes() {
    const std::wstring tail=L"\\Winedows.ImportRouteFixture."+std::to_wstring(GetCurrentProcessId());
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        const std::wstring node=view==KEY_WOW64_32KEY?L"\\WOW6432Node":L"";
        for(const wchar_t *root:{L"Machine",L"User"}){
            std::wstring original=std::wstring(root)+L"\\Software\\Classes\\CLSID"+tail;
            std::wstring expected=std::wstring(root)+L"\\Software\\Classes"+node+L"\\CLSID"+tail;
            check(!_wcsicmp(wd::registry_import_name(original,view).c_str(),expected.c_str()),"Windows import COM view independent of launcher bitness");
            original=std::wstring(root)+L"\\Software\\Classes\\AppID"+tail;
            check(!_wcsicmp(wd::registry_import_name(original,view).c_str(),original.c_str()),"Windows import shared AppID stays shared");
        }
        check(!_wcsicmp(wd::registry_import_name(L"Machine\\Software"+tail,view).c_str(),(L"Machine\\Software"+node+tail).c_str()),"Windows import private machine key follows selected view");
        check(wd::registry_import_name(L"User\\Software"+tail,view)==L"User\\Software"+tail,"Windows import ordinary user key stays shared");
        check(wd::registry_import_name(L"Default\\Software"+tail,view)==L"Default\\Software"+tail,"Windows import default user root stays physical");
    }
}
static void import_check(const std::wstring &token,const std::wstring &clsid) {
    const std::wstring base=L"Software\\WinedowsRegistryTests\\"+token;
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        bool narrow=view==KEY_WOW64_32KEY;
        expect(HKEY_LOCAL_MACHINE,base+L"\\Import",L"View",narrow?L"32":L"64","Windows .reg populated selected machine view",view);
        expect(HKEY_CURRENT_USER,base+L"\\Import",L"Shared",L"64","Windows .reg ordinary user key is shared",view);
        expect(HKEY_LOCAL_MACHINE,base+L"\\WinePhysical",L"View",narrow?L"physical32":L"physical64","Wine text hive retained explicit physical key layout",view);
        expect(HKEY_CLASSES_ROOT,L"CLSID\\"+clsid+L"\\InprocServer32",nullptr,narrow?L"C:\\tests\\registry-machine32.dll":L"C:\\tests\\registry-machine64.dll","imported shared CLSID resolves selected architecture",view);
        absent(HKEY_LOCAL_MACHINE,base+L"\\InvalidImport",view);
    }
    expect(HKEY_LOCAL_MACHINE,base+L"\\DefaultImport",L"View",L"default64","omitted import view remains native64 even on x86 launcher",KEY_WOW64_64KEY);
    absent(HKEY_LOCAL_MACHINE,base+L"\\DefaultImport",KEY_WOW64_32KEY);
    expect(HKEY_CLASSES_ROOT,classes(token)+L".ImportShared",nullptr,L"64","Windows .reg HKCR shared class survives view selection");
    expect(HKEY_CURRENT_USER,L"Software\\Classes\\"+classes(token)+L".ImportUserShared",nullptr,L"64","Windows .reg user shared class survives view selection");
}
struct BackendSnapshot {DWORD magic=0x57444231,types[2]{},sizes[2]{};BYTE values[2][256]{};};
static const wchar_t *backend_keys[]={L"Software\\Microsoft\\OLE",L"Software\\Microsoft\\Rpc"};
static const wchar_t *backend_values[]={L"EnableDCOM",L"ConnectionOptionsFlag"};
static const wchar_t *host_shell_class=L"CLSID\\{00021401-0000-0000-C000-000000000046}";
static bool backend_value(HKEY key,unsigned which,DWORD &type,DWORD &size,BYTE *bytes) {
    size=256;LSTATUS s=RegQueryValueExW(key,backend_values[which],nullptr,&type,bytes,&size);
    check(!s&&size<=256,"read OLE/Rpc discovery scalar",s);return !s&&size<=256;
}
static bool load_backend_snapshot(const wchar_t *path,BackendSnapshot &snapshot) {
    FILE *file=_wfopen(path,L"rb");if(!file){check(false,"open backend snapshot",GetLastError());return false;}
    bool ok=fread(&snapshot,1,sizeof(snapshot),file)==sizeof(snapshot)&&snapshot.magic==0x57444231&&snapshot.sizes[0]<=256&&snapshot.sizes[1]<=256;
    fclose(file);check(ok,"load bounded backend snapshot");return ok;
}
static void backend_snapshot(const std::wstring &token,const wchar_t *path) {
    BackendSnapshot snapshot;std::wstring marker=L"WinedowsRegistryBackend."+token;
    for(unsigned i=0;i<2;i++){
        HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,backend_keys[i],0,KEY_READ,&key);check(!s,"host backend key readable",s);if(s)continue;
        backend_value(key,i,snapshot.types[i],snapshot.sizes[i],snapshot.values[i]);DWORD bytes=0;s=RegQueryValueExW(key,marker.c_str(),nullptr,nullptr,nullptr,&bytes);
        check(s==ERROR_FILE_NOT_FOUND,"host backend marker absent before fixture",s);RegCloseKey(key);
    }
    HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_CLASSES_ROOT,host_shell_class,0,KEY_READ,&key);check(!s,"host class exists for read-through boundary test",s);if(key)RegCloseKey(key);
    FILE *file=_wfopen(path,L"wb");bool ok=file&&fwrite(&snapshot,1,sizeof(snapshot),file)==sizeof(snapshot);if(file)fclose(file);check(ok,"save backend snapshot artifact");
}
static void backend_check(const std::wstring &token,const wchar_t *path,bool mutate,bool host) {
    BackendSnapshot snapshot;if(!load_backend_snapshot(path,snapshot))return;
    std::wstring marker=L"WinedowsRegistryBackend."+token;
    for(unsigned i=0;i<2;i++){
        HKEY key=nullptr;LSTATUS s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,backend_keys[i],0,KEY_READ,&key);check(!s,"OLE/Rpc backend discovery open",s);if(s)continue;
        DWORD type=0,size=0;BYTE data[256]{};
        if(backend_value(key,i,type,size,data))check(type==snapshot.types[i]&&size==snapshot.sizes[i]&&!memcmp(data,snapshot.values[i],size),"backend value matches unchanged native baseline");
        DWORD length=0;s=RegQueryValueExW(key,marker.c_str(),nullptr,nullptr,nullptr,&length);check(s==ERROR_FILE_NOT_FOUND,"backend marker absent outside modified prefix",s);RegCloseKey(key);
        if(!mutate)continue;
        key=nullptr;s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,backend_keys[i],0,KEY_ALL_ACCESS,&key);check(!s,"read-through handle accepts private write intent",s);if(s)continue;
        std::vector<BYTE> value;
        if(snapshot.types[i]==REG_SZ){std::wstring text=L"Winedows private "+token;value.resize((text.size()+1)*2);memcpy(value.data(),text.c_str(),value.size());}
        else if(snapshot.types[i]==REG_DWORD&&snapshot.sizes[i]==4){DWORD number;memcpy(&number,snapshot.values[i],4);number^=0x5a5a5a5a;value.resize(4);memcpy(value.data(),&number,4);}
        else{check(false,"fixture backend scalar type is supported");RegCloseKey(key);continue;}
        s=RegSetValueExW(key,backend_values[i],0,snapshot.types[i],value.data(),(DWORD)value.size());check(!s,"write existing backend setting through read-through handle stays private",s);
        set(key,marker.c_str(),L"private");RegCloseKey(key);
        key=nullptr;s=RegOpenKeyExW(HKEY_LOCAL_MACHINE,backend_keys[i],0,KEY_READ,&key);check(!s,"reopen private backend override",s);
        if(!s){if(backend_value(key,i,type,size,data))check(type==snapshot.types[i]&&size==value.size()&&!memcmp(data,value.data(),size),"private backend setting overrides host value");RegCloseKey(key);}
        expect(HKEY_LOCAL_MACHINE,backend_keys[i],marker.c_str(),L"private","private backend marker persists");
    }
    if(!host)absent(HKEY_CLASSES_ROOT,host_shell_class);
}
static void duplicate_view(const std::wstring &clsid,bool host) {
    std::wstring child=host?L"CLSID":L"CLSID\\"+clsid;
    if(!host)for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY key=create(HKEY_LOCAL_MACHINE,L"Software\\Classes\\"+child,view);if(key){set(key,L"View",view==KEY_WOW64_32KEY?L"32":L"64");RegCloseKey(key);}
    }
    using Duplicate=NTSTATUS(NTAPI*)(HANDLE,HANDLE,HANDLE,HANDLE*,ACCESS_MASK,ULONG,ULONG);
    auto nt_duplicate=(Duplicate)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtDuplicateObject");
    auto open_parent=[](REGSAM view){HKEY key=nullptr;return !RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|view,&key)?key:nullptr;};
    auto correct_view=[&](HANDLE parent,REGSAM view){
        HKEY key=nullptr;if(RegOpenKeyExW((HKEY)parent,child.c_str(),0,KEY_READ,&key))return false;bool result;
        if(host){std::wstring name=queried_name(key);std::transform(name.begin(),name.end(),name.begin(),towlower);bool narrow=name.find(L"\\wow6432node\\clsid")!=name.npos;result=narrow==(view==KEY_WOW64_32KEY);}
        else{wchar_t text[16]{};DWORD bytes=sizeof(text),type=0;LSTATUS s=RegQueryValueExW(key,L"View",nullptr,&type,(BYTE*)text,&bytes);result=!s&&type==REG_SZ&&!wcscmp(text,view==KEY_WOW64_32KEY?L"32":L"64");}
        RegCloseKey(key);return result;
    };
    HANDLE process=OpenProcess(PROCESS_DUP_HANDLE,FALSE,GetCurrentProcessId());check(process!=nullptr,"open same-process handle with duplication rights only",GetLastError());if(!process)return;
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        HKEY original=open_parent(view);check(original!=nullptr,"open requested shared Classes view");if(!original)continue;
        HANDLE first=nullptr,second=nullptr;
        BOOL ok=DuplicateHandle(process,original,process,&first,KEY_READ,FALSE,0);check(ok!=FALSE,"duplicate view with explicit read access and real process handles",ok?0:GetLastError());
        if(ok){check(correct_view(first,view),"duplicate retains requested 32/64 registry view");ok=DuplicateHandle(GetCurrentProcess(),first,GetCurrentProcess(),&second,0,TRUE,DUPLICATE_SAME_ACCESS);check(ok!=FALSE,"duplicate a duplicate",ok?0:GetLastError());if(ok){check(correct_view(second,view),"duplicate chain retains registry view");RegCloseKey((HKEY)second);}RegCloseKey((HKEY)first);}
        HANDLE failed=nullptr;NTSTATUS status=nt_duplicate(GetCurrentProcess(),original,GetCurrentProcess(),&failed,ACCESS_SYSTEM_SECURITY,0,0);
        check(status<0&&!failed,"failed duplication returns no target",status);check(correct_view(original,view),"failed duplication without close preserves source view");RegCloseKey(original);
        original=open_parent(view);first=nullptr;status=nt_duplicate(GetCurrentProcess(),original,process,&first,0,0,DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);
        check(!status&&first,"duplicate and close source",status);if(!status&&first){check(correct_view(first,view),"close-source target retains view");RegCloseKey((HKEY)first);}
        // A failure with CLOSE_SOURCE still closes the source. Do not use the
        // invalid handle afterward; subsequent reuse is exercised below.
        original=open_parent(view);failed=nullptr;status=nt_duplicate(GetCurrentProcess(),original,GetCurrentProcess(),&failed,ACCESS_SYSTEM_SECURITY,0,DUPLICATE_CLOSE_SOURCE);
        check(status<0&&!failed,"failed close-source duplication returns no target",status);
        original=open_parent(view);status=nt_duplicate(GetCurrentProcess(),original,nullptr,nullptr,0,0,DUPLICATE_CLOSE_SOURCE);
        check(!status,"close-only duplication with null target process",status);
        original=open_parent(view);check(SetHandleInformation(original,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)!=FALSE,"protect duplication source");first=nullptr;
        status=nt_duplicate(GetCurrentProcess(),original,GetCurrentProcess(),&first,0,0,DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);
        check(!status&&first,"duplicate protected source",status);check(correct_view(original,view),"protected close-source handle keeps its view");
        if(!status&&first){check(correct_view(first,view),"protected-source duplicate keeps its view");RegCloseKey((HKEY)first);}
        SetHandleInformation(original,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);RegCloseKey(original);
    }
    std::vector<std::pair<HANDLE,REGSAM>> seen;unsigned reused=0;bool views_ok=true;
    for(unsigned i=0;i<64;i++){
        REGSAM view=i%2?KEY_WOW64_64KEY:KEY_WOW64_32KEY;HKEY original=open_parent(view);HANDLE target=nullptr;
        if(!original){views_ok=false;break;}
        BOOL ok=DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&target,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE);
        if(!ok){views_ok=false;break;}
        auto previous=std::find_if(seen.begin(),seen.end(),[&](const auto &item){return item.first==target;});
        if(previous!=seen.end()){if(previous->second!=view)++reused;previous->second=view;}else seen.push_back({target,view});
        views_ok=correct_view(target,view)&&views_ok;RegCloseKey((HKEY)target);
    }
    printf("Observed opposite-view handle reuse: %u\n",reused);
    check(reused>0,"handle reuse actually occurred during duplicate stress");check(views_ok,"reused handles never retain an earlier registry view");
    CloseHandle(process);
    puts("NOT TESTED: cross-process duplicated or inherited registry handle view propagation.");
}
static void duplicate_errors() {
    using Duplicate=NTSTATUS(NTAPI*)(HANDLE,HANDLE,HANDLE,HANDLE*,ACCESS_MASK,ULONG,ULONG);
    auto duplicate=(Duplicate)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtDuplicateObject");
    for(unsigned scenario=0;scenario<5;scenario++){
        HKEY source=nullptr;LSTATUS opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|KEY_WOW64_32KEY,&source);check(!opened,"open native duplicate failure fixture",opened);if(opened)continue;
        HANDLE target=nullptr,process=scenario==2?(HANDLE)(ULONG_PTR)0xdeadbee0:scenario==3?nullptr:GetCurrentProcess();
        ACCESS_MASK access=scenario<2?ACCESS_SYSTEM_SECURITY:0;
        ULONG options=scenario<2?0:DUPLICATE_SAME_ACCESS;if(scenario)options|=DUPLICATE_CLOSE_SOURCE;
        if(scenario==4)check(SetHandleInformation(source,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)!=FALSE,"protect native duplicate source");
        printf("DUPLICATE-BASELINE begin case=%u source=%p\n",scenario,source);fflush(stdout);
        NTSTATUS status=duplicate(GetCurrentProcess(),source,process,scenario==3?nullptr:&target,access,0,options);
        printf("DUPLICATE-BASELINE returned case=%u status=%08lx\n",scenario,(ULONG)status);fflush(stdout);
        DWORD source_flags=0;NTSTATUS source_status=(scenario==0||scenario==4)?(GetHandleInformation(source,&source_flags)?0:(NTSTATUS)GetLastError()):(NTSTATUS)0xffffffff;
        printf("DUPLICATE-BASELINE case=%u status=%08lx source_status=%08lx target=%p\n",scenario,(ULONG)status,(ULONG)source_status,target);
        fflush(stdout);
        if(!source_status){SetHandleInformation(source,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);RegCloseKey(source);}
        if(status>=0&&target)RegCloseKey((HKEY)target);
    }
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc!=4){fputs("registry-probe MODE SUITE-TOKEN CLSID\n",stderr);return 2;}
    std::wstring mode=argv[1],token=argv[2],clsid=argv[3];
    bool injected=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")!=nullptr;
    bool host=mode.rfind(L"--host-",0)==0;
    if(!host&&!injected){fputs("Use the Winedows loader for mutating fixtures.\n",stderr);return 2;}
    if(host&&injected){fputs("Host audit must run outside the loader.\n",stderr);return 2;}
    query_key=(QueryKey)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryKey");open_key=(OpenKey)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtOpenKey");
    if(!query_key||!open_key)return 2;
    if(mode==L"--host-names")host_names();
    else if(mode==L"--host-views")host_views();
    else if(mode==L"--host-route-tests")host_route_tests();
    else if(mode==L"--host-import-routes")host_import_routes();
    else if(mode==L"--import-check")import_check(token,clsid);
    else if(mode==L"--host-backend-snapshot")backend_snapshot(token,argv[3]);
    else if(mode==L"--host-backend-unchanged"||mode==L"--backend-read"||mode==L"--backend-check")backend_check(token,argv[3],mode==L"--backend-check",host);
    else if(mode==L"--host-duplicate-view"||mode==L"--duplicate-view")duplicate_view(clsid,host);
    else if(mode==L"--host-duplicate-errors")duplicate_errors();
    else if(mode==L"--wow64-views")wow64_views(token);
    else if(mode==L"--names-classes")names_and_classes(token);
    else if(mode==L"--seed-machine"||mode==L"--seed-user")register_server(clsid,mode==L"--seed-user");
    else if(mode==L"--activate-machine"||mode==L"--activate-user")activate(clsid,mode==L"--activate-user");
    else if(mode==L"--host-absent"||mode==L"--absent")audit_absent(token,clsid);
    else return 2;
    printf("Registry %zu-bit %ls: %d failure(s)\n",sizeof(void*)*8,mode.c_str(),failures);return failures?1:0;
}
#endif
