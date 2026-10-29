// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
#include "video-backend.h"
#include "registry-views.h"
static const wchar_t *paths[]={
    L"Software\\Classes\\CLSID\\{e79167d7-1b85-4d78-b603-798e0e1a4c67}",
    L"Software\\Classes\\CLSID\\{48E2ED0F-98C2-4A37-BED5-166312DDD83F}",
    L"Software\\Classes\\CLSID\\{477EC299-1421-4bdd-971F-7CCB933F21AD}",
    L"Software\\Classes\\CLSID\\{271C3902-6095-4c45-A22F-20091816EE9E}",
    L"Software\\Classes\\CLSID\\{62CE7E72-4C71-4d20-B15D-452831A87D9D}",
    L"Software\\Classes\\CLSID\\{32D186A7-218F-4C75-8876-DD77273A8999}",
    L"Software\\Classes\\CLSID\\{88753B26-5B24-49BD-B2E7-0C445C78C982}",
    L"Software\\Microsoft\\Windows Media Foundation\\SchemeHandlers\\file:",
    L"Software\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\.mp4",
    L"Software\\Classes\\MediaFoundation\\Transforms\\62ce7e72-4c71-4d20-b15d-452831a87d9d",
    L"Software\\Classes\\MediaFoundation\\Transforms\\Categories\\d6c02d4b-6833-45b4-971a-05a4b04bab91\\62ce7e72-4c71-4d20-b15d-452831a87d9d",
    L"Software\\Classes\\MediaFoundation\\Transforms\\32d186a7-218f-4c75-8876-dd77273a8999",
    L"Software\\Classes\\MediaFoundation\\Transforms\\Categories\\9ea73fb4-ef7a-4559-8d5d-719d8f0426c7\\32d186a7-218f-4c75-8876-dd77273a8999",
    L"Software\\Classes\\MediaFoundation\\Transforms\\88753b26-5b24-49bd-b2e7-0c445c78c982",
    L"Software\\Classes\\MediaFoundation\\Transforms\\Categories\\302ea3fc-aa5f-47f9-9f7a-c2188bb16302\\88753b26-5b24-49bd-b2e7-0c445c78c982"
};
static unsigned failures;
static void check(bool ok,const char *name){printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;}
static std::wstring path(bool user,const wchar_t *name,REGSAM view){return wd::registry_import_name(std::wstring(user?L"User\\":L"Machine\\")+name,view);}
static bool exists(HKEY hive,const std::wstring &name){HKEY key=nullptr;LSTATUS result=RegOpenKeyExW(hive,name.c_str(),0,KEY_READ,&key);if(key)RegCloseKey(key);return !result;}
static void clear(HKEY hive){for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(bool user:{false,true})for(auto name:paths){LSTATUS result=RegDeleteTreeW(hive,path(user,name,view).c_str());check(!result||result==ERROR_FILE_NOT_FOUND||result==ERROR_PATH_NOT_FOUND,"clear known disposable video registration");}}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3||wcscmp(argv[1],L"--disposable-hive"))return 2;
    std::wstring file=argv[2];if(file.find(L"test-output\\video-")==file.npos||file.size()<13||file.substr(file.size()-13)!=L"\\registry.dat")return 2;
    HKEY hive=nullptr;LSTATUS result=RegLoadAppKeyW(file.c_str(),&hive,KEY_ALL_ACCESS,0,0);check(!result,"open disposable video app hive");if(result)return 1;
    try{
        for(bool user:{false,true})for(bool empty:{false,true}){
            clear(hive);
            for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(auto name:paths){
                HKEY key=nullptr;result=RegCreateKeyExW(hive,path(user,name,view).c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr);check(!result,"seed private video override");
                if(key){if(!empty){const wchar_t marker[]=L"private-video-override";check(!RegSetValueExW(key,nullptr,0,REG_SZ,(const BYTE*)marker,sizeof(marker)),"set private video override value");}RegCloseKey(key);}
            }
            wd::initialize_video_backend(hive);
            for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(auto name:paths){
                HKEY key=nullptr;result=RegOpenKeyExW(hive,path(user,name,view).c_str(),0,KEY_READ,&key);check(!result,"explicit video key remains present");
                if(key){DWORD children=0,values=0;result=RegQueryInfoKeyW(key,nullptr,nullptr,nullptr,&children,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);check(!result&&children==0&&values==(empty?0u:1u),"empty or broken video registration is not populated");
                    if(!empty){wchar_t value[64]{};DWORD size=sizeof(value);result=RegQueryValueExW(key,nullptr,nullptr,nullptr,(BYTE*)value,&size);check(!result&&!wcscmp(value,L"private-video-override"),"private video value is unchanged");}RegCloseKey(key);}
                if(user)check(!exists(hive,path(false,name,view)),"user video override prevents machine default");
            }
        }
        clear(hive);wd::initialize_video_backend(hive);
        for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})for(auto name:paths){check(exists(hive,path(false,name,view)),"absent video registration receives default");check(!exists(hive,path(true,name,view)),"default does not create user registration");}
        wd::initialize_video_backend(hive);check(true,"reinitialization succeeds");
    }catch(const std::exception &error){printf("FAIL %s\n",error.what());++failures;}
    RegFlushKey(hive);RegCloseKey(hive);printf("VIDEO DEFAULTS failures=%u\n",failures);return failures?1:0;
}
