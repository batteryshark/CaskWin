// SPDX-License-Identifier: LGPL-2.1-or-later
#include "imaging-defaults.h"
#include "registry-views.h"
#include <string>
#include <stdexcept>
#include <cwchar>
#include <vector>

namespace wd {
namespace {
// These identities are defined by Wine's include/wincodec.idl and its
// windowscodecs registration code. The native backend supplies their OS DLL.
struct ImagingClass {const wchar_t *clsid,*name;};
const ImagingClass classes[]={
    {L"{CACAF262-9370-4615-A13B-9F5539DA4C0A}",L"WICImagingFactory"},
    {L"{389EA17B-5078-4CDE-B6EF-25C15175C751}",L"WIC PNG Decoder"},
    {L"{9456A480-E88B-43EA-9E73-0B2D9B71B1CA}",L"WIC JPEG Decoder"}
};
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
void checked(LSTATUS status,const char *message){if(status)throw std::runtime_error(std::string(message)+" ("+std::to_string(status)+")");}
bool exists(HKEY hive,const std::wstring &path){
    Key key;LSTATUS status=RegOpenKeyExW(hive,path.c_str(),0,KEY_READ,&key.value);
    if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)return false;
    checked(status,"Inspect private imaging class");return true;
}
bool create_absent(HKEY root,const std::wstring &path,Key &key){
    DWORD disposition=0;checked(RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key.value,&disposition),"Create private imaging class");
    return disposition==REG_CREATED_NEW_KEY;
}
void set(HKEY key,const wchar_t *name,const wchar_t *value){
    checked(RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t))),"Write private imaging class");
}
std::wstring native_module(const wchar_t *clsid,REGSAM view){
    // Read one exact machine class. Never enumerate codec catalogs or import a
    // host user registration / third-party COM server into an application hive.
    Key key;std::wstring path=std::wstring(L"Software\\Classes\\CLSID\\")+clsid+L"\\InprocServer32";
    checked(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_QUERY_VALUE|view,&key.value),"Read native WIC class");
    std::vector<wchar_t> value(32768),expanded(32768),system(512),wow(512);DWORD type=0,size=(DWORD)(value.size()*sizeof(wchar_t));
    checked(RegQueryValueExW(key.value,nullptr,nullptr,&type,(BYTE*)value.data(),&size),"Read native WIC module");
    if((type!=REG_SZ&&type!=REG_EXPAND_SZ)||size<sizeof(wchar_t)||size>value.size()*sizeof(wchar_t)||size%sizeof(wchar_t)||value[size/sizeof(wchar_t)-1])throw std::runtime_error("Invalid native WIC module registration");
    DWORD expanded_size=ExpandEnvironmentStringsW(value.data(),expanded.data(),32768);
    if(!expanded_size||expanded_size>32768)throw std::runtime_error("Cannot expand native WIC module path");
    // GetSystemWow64DirectoryW reports success without filling a 32768-char
    // buffer on the tested Windows build; use a bounded system-path buffer.
    constexpr UINT system_capacity=512;
    UINT system_size=GetSystemDirectoryW(system.data(),system_capacity),wow_size=GetSystemWow64DirectoryW(wow.data(),system_capacity);
    if(!system_size||system_size>=system_capacity||!system[0]||wow_size>=system_capacity||(wow_size&&!wow[0]))throw std::runtime_error("Cannot locate native imaging system directory");
    std::wstring system_dll=std::wstring(system.data())+L"\\windowscodecs.dll",wow_dll=wow_size?std::wstring(wow.data())+L"\\windowscodecs.dll":system_dll;
    // Permit only the OS component, then record an absolute system path so a
    // game-local DLL cannot unexpectedly become the default COM implementation.
    if(_wcsicmp(expanded.data(),L"windowscodecs.dll")&&_wcsicmp(expanded.data(),system_dll.c_str())&&_wcsicmp(expanded.data(),wow_dll.c_str()))throw std::runtime_error("Unsupported native WIC module registration");
    return view==KEY_WOW64_32KEY?wow_dll:system_dll;
}
}
void initialize_imaging_com(HKEY hive){
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(const auto &entry:classes){
        auto machine=registry_import_name(std::wstring(L"Machine\\Software\\Classes\\CLSID\\")+entry.clsid,view);
        auto user=registry_import_name(std::wstring(L"User\\Software\\Classes\\CLSID\\")+entry.clsid,view);
        // An existing entire class key is authoritative, including empty or
        // deliberately broken user and machine registrations.
        if(exists(hive,machine)||exists(hive,user))continue;
        auto module=native_module(entry.clsid,view);Key cls;
        if(!create_absent(hive,machine,cls))continue;
        set(cls.value,nullptr,entry.name);Key server;create_absent(cls.value,L"InprocServer32",server);
        set(server.value,nullptr,module.c_str());set(server.value,L"ThreadingModel",L"Both");
    }
    // Native WIC has built-in encoders but validates this catalog container when
    // writing PNG metadata. Its absence makes WritePixels fail with
    // WINCODEC_ERR_INVALIDREGISTRATION. The prefix starts with an empty catalog;
    // do not enumerate or copy host codec/category registrations into it.
    constexpr auto categories=L"{FAE3D380-FEA4-4623-8C75-C6B61110B681}";
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        auto machine=registry_import_name(std::wstring(L"Machine\\Software\\Classes\\CLSID\\")+categories,view);
        auto user=registry_import_name(std::wstring(L"User\\Software\\Classes\\CLSID\\")+categories,view);
        // An intentionally empty private class also remains authoritative.
        if(exists(hive,machine)||exists(hive,user))continue;
        Key cls;if(!create_absent(hive,machine,cls))continue;
        Key instances;create_absent(cls.value,L"Instance",instances);
    }
}
}
