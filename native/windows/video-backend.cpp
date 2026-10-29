// SPDX-License-Identifier: LGPL-2.1-or-later
#include "video-backend.h"
#include "registry-views.h"
#include <string>
#include <vector>
#include <stdexcept>

namespace wd {
namespace {
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
struct Class {const wchar_t *id,*name,*module;};
// File/MP4/H.264 identities also appear in Wine's mf/mfreadwrite/decoder IDL.
// Media Source Activate is an additional native MFStartup requirement.
const Class classes[]={
    {L"{e79167d7-1b85-4d78-b603-798e0e1a4c67}",L"MF Media Source Activate",L"mfcore.dll"},
    {L"{48E2ED0F-98C2-4A37-BED5-166312DDD83F}",L"MFReadWrite Class Factory",L"mfreadwrite.dll"},
    {L"{477EC299-1421-4bdd-971F-7CCB933F21AD}",L"File Scheme Handler",L"mfcore.dll"},
    {L"{271C3902-6095-4c45-A22F-20091816EE9E}",L"MPEG4 Byte Stream Handler",L"mfmp4srcsnk.dll"},
    {L"{62CE7E72-4C71-4d20-B15D-452831A87D9D}",L"Microsoft H264 Video Decoder MFT",L"msmpeg2vdec.dll"},
    {L"{32D186A7-218F-4C75-8876-DD77273A8999}",L"Microsoft AAC Audio Decoder MFT",L"MSAudDecMFT.dll"},
    {L"{88753B26-5B24-49BD-B2E7-0C445C78C982}",L"Microsoft Video Processor MFT",L"msvproc.dll"}
};
struct Transform {size_t class_index;const wchar_t *id,*category;};
const Transform transforms[]={
    {4,L"62ce7e72-4c71-4d20-b15d-452831a87d9d",L"d6c02d4b-6833-45b4-971a-05a4b04bab91"},
    {5,L"32d186a7-218f-4c75-8876-dd77273a8999",L"9ea73fb4-ef7a-4559-8d5d-719d8f0426c7"},
    {6,L"88753b26-5b24-49bd-b2e7-0c445c78c982",L"302ea3fc-aa5f-47f9-9f7a-c2188bb16302"}
};
void checked(LSTATUS result,const char *what){if(result)throw std::runtime_error(std::string(what)+" ("+std::to_string(result)+")");}
bool exists(HKEY root,const std::wstring &path){Key key;LSTATUS result=RegOpenKeyExW(root,path.c_str(),0,KEY_READ,&key.value);if(result==ERROR_FILE_NOT_FOUND||result==ERROR_PATH_NOT_FOUND)return false;checked(result,"Inspect private video registration");return true;}
bool private_exists(HKEY hive,const std::wstring &path,REGSAM view){return exists(hive,registry_import_name(L"Machine\\"+path,view))||exists(hive,registry_import_name(L"User\\"+path,view));}
bool create_absent(HKEY root,const std::wstring &path,Key &key){DWORD disposition=0;checked(RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key.value,&disposition),"Create private video registration");return disposition==REG_CREATED_NEW_KEY;}
void text(HKEY key,const wchar_t *name,const wchar_t *value){checked(RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t))),"Write private video registration");}
std::wstring native_module(const Class &entry,REGSAM view){
    Key key;auto path=std::wstring(L"Software\\Classes\\CLSID\\")+entry.id+L"\\InprocServer32";
    // MF is optional on some Windows editions. Missing or replaced native
    // classes are not imported, and do not prevent creating a general prefix.
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_QUERY_VALUE|view,&key.value))return {};
    wchar_t value[1024]{},expanded[1024]{},system[512]{},wow[512]{};DWORD type=0,size=sizeof(value);
    if(RegQueryValueExW(key.value,nullptr,nullptr,&type,(BYTE*)value,&size)||
       (type!=REG_SZ&&type!=REG_EXPAND_SZ)||size<sizeof(wchar_t)||size>sizeof(value)||size%sizeof(wchar_t)||value[size/sizeof(wchar_t)-1])return {};
    UINT length=ExpandEnvironmentStringsW(value,expanded,1024);if(!length||length>1024)return {};
    UINT system_length=GetSystemDirectoryW(system,512),wow_length=GetSystemWow64DirectoryW(wow,512);
    if(!system_length||system_length>=512||!system[0]||wow_length>=512||(wow_length&&!wow[0]))return {};
    auto system_path=std::wstring(system)+L"\\"+entry.module,wow_path=wow_length?std::wstring(wow)+L"\\"+entry.module:system_path;
    if(_wcsicmp(expanded,entry.module)&&_wcsicmp(expanded,system_path.c_str())&&_wcsicmp(expanded,wow_path.c_str()))return {};
    auto selected=view==KEY_WOW64_32KEY?wow_path:system_path;
    DWORD attributes=GetFileAttributesW(selected.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    return selected;
}
void mapping(HKEY hive,REGSAM view,const wchar_t *path,const Class &entry){
    if(private_exists(hive,path,view))return;
    Key key;if(!create_absent(hive,registry_import_name(std::wstring(L"Machine\\")+path,view),key))return;
    text(key.value,entry.id,entry.name);
}
void transform_catalog(HKEY hive,REGSAM view,const Transform &entry){
    const auto transform=std::wstring(L"Software\\Classes\\MediaFoundation\\Transforms\\")+entry.id;
    const auto category=std::wstring(L"Software\\Classes\\MediaFoundation\\Transforms\\Categories\\")+entry.category+L"\\"+entry.id;
    if(!private_exists(hive,transform,view)){
        Key native;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,transform.c_str(),0,KEY_QUERY_VALUE|view,&native.value))return;
        struct Types {std::vector<BYTE> input,output;} types;
        for(auto pair:{std::pair<const wchar_t*,std::vector<BYTE>*>(L"InputTypes",&types.input),{L"OutputTypes",&types.output}}){
            DWORD type=0,size=0;if(RegQueryValueExW(native.value,pair.first,nullptr,&type,nullptr,&size)||type!=REG_BINARY||!size||size>4096||size%32)return;
            pair.second->resize(size);if(RegQueryValueExW(native.value,pair.first,nullptr,&type,pair.second->data(),&size)||type!=REG_BINARY||size!=pair.second->size())return;
        }
        Key key;if(create_absent(hive,registry_import_name(std::wstring(L"Machine\\")+transform,view),key)){
            text(key.value,nullptr,classes[entry.class_index].name);
            checked(RegSetValueExW(key.value,L"InputTypes",0,REG_BINARY,types.input.data(),(DWORD)types.input.size()),"Write private media input formats");
            checked(RegSetValueExW(key.value,L"OutputTypes",0,REG_BINARY,types.output.data(),(DWORD)types.output.size()),"Write private media output formats");
        }
    }
    if(!private_exists(hive,category,view)){Key key;create_absent(hive,registry_import_name(std::wstring(L"Machine\\")+category,view),key);}
}
}
void initialize_video_backend(HKEY hive){
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        bool available[sizeof(classes)/sizeof(classes[0])]{};
        for(size_t i=0;i<sizeof(classes)/sizeof(classes[0]);i++){
            const auto &entry=classes[i];auto module=native_module(entry,view);available[i]=!module.empty();
            auto path=std::wstring(L"Software\\Classes\\CLSID\\")+entry.id;
            if(module.empty()||private_exists(hive,path,view))continue;
            Key key;if(!create_absent(hive,registry_import_name(L"Machine\\"+path,view),key))continue;
            text(key.value,nullptr,entry.name);Key server;create_absent(key.value,L"InprocServer32",server);
            text(server.value,nullptr,module.c_str());text(server.value,L"ThreadingModel",L"Both");
        }
        if(available[2])mapping(hive,view,L"Software\\Microsoft\\Windows Media Foundation\\SchemeHandlers\\file:",classes[2]);
        if(available[3])mapping(hive,view,L"Software\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\.mp4",classes[3]);
        for(const auto &entry:transforms)if(available[entry.class_index])transform_catalog(hive,view,entry);
    }
}
}
