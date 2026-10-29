// SPDX-License-Identifier: LGPL-2.1-or-later
// Destructive only to known WIC defaults in a disposable app hive.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdio>
#include "imaging-defaults.h"
#include "registry-views.h"

static const wchar_t *classes[]={L"{CACAF262-9370-4615-A13B-9F5539DA4C0A}",L"{389EA17B-5078-4CDE-B6EF-25C15175C751}",L"{9456A480-E88B-43EA-9E73-0B2D9B71B1CA}"};
static const wchar_t *categories=L"{FAE3D380-FEA4-4623-8C75-C6B61110B681}";
static int failures;
static void check(bool ok,const char *label,LSTATUS status=0){printf("%s %s (status=%ld)\n",ok?"PASS":"FAIL",label,status);if(!ok)++failures;}
static std::wstring path(bool user,const wchar_t *clsid,REGSAM view){return wd::registry_import_name(std::wstring(user?L"User\\Software\\Classes\\CLSID\\":L"Machine\\Software\\Classes\\CLSID\\")+clsid,view);}
static bool exists(HKEY hive,const std::wstring &name){HKEY key=nullptr;LSTATUS status=RegOpenKeyExW(hive,name.c_str(),0,KEY_READ,&key);if(key)RegCloseKey(key);return status==ERROR_SUCCESS;}
static std::wstring value(HKEY hive,const std::wstring &name){HKEY key=nullptr;if(RegOpenKeyExW(hive,name.c_str(),0,KEY_QUERY_VALUE,&key))return L"";wchar_t data[32768]{};DWORD size=sizeof(data),type=0;LSTATUS status=RegQueryValueExW(key,nullptr,nullptr,&type,(BYTE*)data,&size);RegCloseKey(key);return status?L"":std::wstring(data);}
static void clear(HKEY hive){for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(const auto clsid:classes)for(bool user:{false,true}){auto name=path(user,clsid,view);LSTATUS status=RegDeleteTreeW(hive,name.c_str());check(status==ERROR_SUCCESS||status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND,"clear disposable imaging registration",status);}}
static void seed(HKEY hive,bool user,bool empty){
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(const auto clsid:classes){
        auto name=path(user,clsid,view);HKEY key=nullptr;LSTATUS status=RegCreateKeyExW(hive,name.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);check(!status,"seed explicit private class",status);
        if(key&&!empty){HKEY server=nullptr;status=RegCreateKeyExW(key,L"InprocServer32",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&server,nullptr);if(!status){const wchar_t custom[]=L"C:\\tests\\private-imaging-server.dll";status=RegSetValueExW(server,nullptr,0,REG_SZ,(const BYTE*)custom,sizeof(custom));RegCloseKey(server);}check(!status,"seed private server override",status);}
        if(key)RegCloseKey(key);
    }
}
static void preserved(HKEY hive,bool user,bool empty){
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(const auto clsid:classes){
        auto name=path(user,clsid,view);check(exists(hive,name),"explicit private class remains present");
        if(empty)check(!exists(hive,name+L"\\InprocServer32"),"empty class is not populated");
        else check(value(hive,name+L"\\InprocServer32")==L"C:\\tests\\private-imaging-server.dll","private server override remains unchanged");
        if(user)check(!exists(hive,path(false,clsid,view)),"private user class prevents implicit machine default");
    }
}
static void clear_categories(HKEY hive){
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(bool user:{false,true}){
        auto name=path(user,categories,view);LSTATUS status=RegDeleteTreeW(hive,name.c_str());
        check(!status||status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND,"clear disposable WIC category catalog",status);
    }
}
static void category_defaults(HKEY hive){
    for(bool user:{false,true})for(bool empty:{false,true}){
        clear_categories(hive);
        for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
            auto name=path(user,categories,view);HKEY key=nullptr;
            LSTATUS status=RegCreateKeyExW(hive,name.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);
            check(!status,"seed explicit private WIC catalog",status);
            if(key&&!empty){HKEY entry=nullptr;status=RegCreateKeyExW(key,L"Instance\\PrivateFixture",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&entry,nullptr);
                if(!status){const wchar_t marker[]=L"preserve private codec";status=RegSetValueExW(entry,nullptr,0,REG_SZ,(const BYTE*)marker,sizeof(marker));RegCloseKey(entry);}
                check(!status,"seed private category entry",status);}
            if(key)RegCloseKey(key);
        }
        wd::initialize_imaging_com(hive);
        for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
            auto name=path(user,categories,view);check(exists(hive,name),"private category class remains present");
            if(empty)check(!exists(hive,name+L"\\Instance"),"empty category class is not populated");
            else check(value(hive,name+L"\\Instance\\PrivateFixture")==L"preserve private codec","private category entry remains unchanged");
            if(user)check(!exists(hive,path(false,categories,view)),"private user category prevents machine default");
        }
    }
    clear_categories(hive);wd::initialize_imaging_com(hive);wd::initialize_imaging_com(hive);
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        auto name=path(false,categories,view)+L"\\Instance";HKEY key=nullptr;
        LSTATUS status=RegOpenKeyExW(hive,name.c_str(),0,KEY_READ,&key);check(!status,"absent category receives Instance container",status);
        if(key){DWORD subkeys=1,values=1;status=RegQueryInfoKeyW(key,nullptr,nullptr,nullptr,&subkeys,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);
            check(!status&&!subkeys&&!values,"new category catalog is empty and imports no host entries",status);RegCloseKey(key);}
        check(!exists(hive,path(true,categories,view)),"category default does not create user registration");
    }
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3||wcscmp(argv[1],L"--disposable-hive")){fputs("imaging-defaults-probe --disposable-hive PRIVATE_REGISTRY_DAT\n",stderr);return 2;}
    std::wstring file=argv[2];
    if(file.find(L"test-output\\imaging-")==file.npos||file.size()<13||file.substr(file.size()-13)!=L"\\registry.dat"){fputs("Refusing non-imaging test hive path\n",stderr);return 2;}
    HKEY hive=nullptr;LSTATUS status=RegLoadAppKeyW(file.c_str(),&hive,KEY_ALL_ACCESS,0,0);check(!status&&hive,"open disposable private app hive",status);if(status)return 1;
    try{
        for(bool user:{false,true})for(bool empty:{false,true}){clear(hive);seed(hive,user,empty);wd::initialize_imaging_com(hive);preserved(hive,user,empty);}
        clear(hive);wd::initialize_imaging_com(hive);
        for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(const auto clsid:classes){
            wchar_t directory[MAX_PATH]{};if(view==KEY_WOW64_32KEY&&!GetSystemWow64DirectoryW(directory,MAX_PATH))GetSystemDirectoryW(directory,MAX_PATH);else if(view==KEY_WOW64_64KEY)GetSystemDirectoryW(directory,MAX_PATH);
            auto expected=std::wstring(directory)+L"\\windowscodecs.dll";
            check(!_wcsicmp(value(hive,path(false,clsid,view)+L"\\InprocServer32").c_str(),expected.c_str()),"absent class receives the correct architecture's system imaging DLL");
            check(!exists(hive,path(true,clsid,view)),"default initialization does not create user classes");
        }
        wd::initialize_imaging_com(hive);check(true,"default initialization is idempotent");
        category_defaults(hive);
    }catch(const std::exception &error){printf("FAIL %s\n",error.what());++failures;}
    RegFlushKey(hive);RegCloseKey(hive);printf("Imaging defaults %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
