// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#ifdef WINEDOWS_SYSTEM_LOADER_MARKER
extern "C" __declspec(dllexport) DWORD WINAPI WinedowsSystemLoaderMarker(){return 0x57444944;}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#else
static int failures;
static void check(bool ok,const char *what,DWORD error=0){printf("%s %s error=%lu\n",ok?"PASS":"FAIL",what,error);if(!ok)++failures;}
static std::wstring module_path(HMODULE module){wchar_t value[32768];DWORD size=GetModuleFileNameW(module,value,32768);return size&&size<32768?std::wstring(value,size):std::wstring();}
static std::wstring system_path(const wchar_t *name){
    wchar_t directory[MAX_PATH]{};UINT count=sizeof(void*)==4?GetSystemWow64DirectoryW(directory,MAX_PATH):0;
    if(!count)count=GetSystemDirectoryW(directory,MAX_PATH);
    return count&&count<MAX_PATH?std::wstring(directory)+L"\\"+name:std::wstring();
}
static bool system_module(HMODULE module,const wchar_t *name){
    std::wstring actual=module_path(module),expected=system_path(name);
    if(!_wcsicmp(actual.c_str(),expected.c_str()))return true;
    wchar_t directory[MAX_PATH];GetSystemDirectoryW(directory,MAX_PATH);
    expected=std::wstring(directory)+L"\\"+name;
    return !_wcsicmp(actual.c_str(),expected.c_str());
}
int wmain(int argc,wchar_t **argv){
    if(argc!=2)return 2;
    std::wstring mode=argv[1];
    if(mode==L"--raw-local"){
        HMODULE module=LoadLibraryExW(system_path(L"xinput1_4.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module!=nullptr,"native absolute system load",GetLastError());if(!module)return 1;
        auto marker=(DWORD(WINAPI*)())GetProcAddress(module,"WinedowsSystemLoaderMarker");
        bool redirected=marker&&marker()==0x57444944;
        wprintf(L"Native absolute load resolved: %ls\n",module_path(module).c_str());
        FreeLibrary(module);
        if(!redirected){puts("UNAVAILABLE: this process does not exercise DotLocal redirection; host policy unchanged.");return 77;}
        puts("PASS DotLocal redirects absolute system DLL to benign application marker");return 0;
    }
    if(mode==L"--family-native"){
        for(auto name:{L"d3d11.dll",L"dxgi.dll",L"d3d10core.dll",L"d3d10_1.dll",L"d3d10.dll"}){
            HMODULE module=LoadLibraryW(name);check(module!=nullptr,"load fallback graphics family member",GetLastError());
            if(module){wprintf(L"Family %ls: %ls\n",name,module_path(module).c_str());check(system_module(module,name),"graphics family remains entirely native");FreeLibrary(module);}
        }
        check(GetModuleHandleW(L"wined3d.dll")==nullptr,"failed builtin preflight did not preload Wine dependencies");
        return failures?1:0;
    }
    HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
    auto backend=shim?(HMODULE(WINAPI*)())GetProcAddress(shim,"WinedowsLoadSystemXInput"):nullptr;
    check(backend!=nullptr,"system backend loader export");if(!backend)return 1;
    bool reject=mode==L"--reject-local";
    if(!reject&&mode!=L"--accept")return 2;
    std::wstring executable=module_path(nullptr),local=executable.substr(0,executable.find_last_of(L"\\/"))+L"\\xinput1_4.dll";
    for(int i=0;i<3;++i){
        SetLastError(4321);HMODULE module=backend();DWORD error=GetLastError();
        if(reject){
            check(!module&&error==ERROR_INVALID_DLL,"redirected backend rejected with stable ERROR_INVALID_DLL",error);
            if(module)FreeLibrary(module);
            check(!GetModuleHandleW(local.c_str()),"rejected backend reference released");
        }else{
            check(module&&system_module(module,L"xinput1_4.dll"),"actual system backend accepted",error);
            if(module){check(!GetProcAddress(module,"WinedowsSystemLoaderMarker"),"backend is not the marker image");FreeLibrary(module);}
        }
    }
    return failures?1:0;
}
#endif
