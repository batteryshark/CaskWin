// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cwctype>
#ifdef WINEDOWS_STATIC_LOADER_IMPORTS
extern "C" {
__declspec(dllimport) HRESULT WINAPI DirectDrawCreate(void*,void**,void*);
__declspec(dllimport) void *WINAPI Direct3DCreate8(UINT);
__declspec(dllimport) void *WINAPI Direct3DCreate9(UINT);
__declspec(dllimport) HRESULT WINAPI DirectInputCreateA(HINSTANCE,DWORD,void**,void*);
__declspec(dllimport) HRESULT WINAPI DirectInput8Create(HINSTANCE,DWORD,void*,void**,void*);
__declspec(dllimport) HRESULT WINAPI DirectSoundCreate(void*,void**,void*);
}
static FARPROC volatile imports[]={reinterpret_cast<FARPROC>(DirectDrawCreate),
    reinterpret_cast<FARPROC>(Direct3DCreate8),reinterpret_cast<FARPROC>(Direct3DCreate9),
    reinterpret_cast<FARPROC>(DirectInputCreateA),reinterpret_cast<FARPROC>(DirectInput8Create),
    reinterpret_cast<FARPROC>(DirectSoundCreate)};
#endif
using str=std::wstring;
static unsigned failures;
static void check(bool ok,const char *what){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",what,ok?0:GetLastError());if(!ok)++failures;}
static str module_path(HMODULE module){wchar_t path[32768];DWORD count=GetModuleFileNameW(module,path,32768);return count&&count<32768?str(path,count):str();}
static std::string narrow(const str &text){int size=WideCharToMultiByte(CP_ACP,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);std::string out(size,0);WideCharToMultiByte(CP_ACP,0,text.c_str(),-1,out.data(),size,nullptr,nullptr);return out;}
static bool equal(const str &left,const str &right){return !_wcsicmp(left.c_str(),right.c_str());}
#ifdef WINEDOWS_STATIC_LOADER_IMPORTS
static FARPROC imported(const char *module,const char *symbol){
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!rva)return nullptr;
    for(auto d=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);d->Name;d++){
        if(_stricmp((char*)base+d->Name,module))continue;
        auto names=(IMAGE_THUNK_DATA*)(base+d->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+d->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)){
            auto entry=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)entry->Name,symbol))return (FARPROC)values->u1.Function;
        }
    }
    return nullptr;
}
#endif
static void resource(const str &path,DWORD flags,bool ansi){
    HMODULE loaded=ansi?LoadLibraryExA(narrow(path).c_str(),nullptr,flags):LoadLibraryExW(path.c_str(),nullptr,flags);
    check(loaded!=nullptr,"resource/datafile load succeeds");
    if(!loaded)return;
    HRSRC info=FindResourceW(loaded,MAKEINTRESOURCEW(101),RT_RCDATA);
    HGLOBAL data=info?LoadResource(loaded,info):nullptr;
    auto bytes=data?(const char*)LockResource(data):nullptr;
    constexpr char marker[]="winedows-path-marker";
    check(bytes&&SizeofResource(loaded,info)>=sizeof(marker)-1&&!memcmp(bytes,marker,sizeof(marker)-1),"resource load preserves requested file");
    check(!!FreeLibrary(loaded),"release resource mapping");
}
static DWORD WINAPI timeout(void*){Sleep(45000);fprintf(stderr,"FAIL loader paths fixture timed out\n");TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,timeout,nullptr,0,nullptr));
    if(argc<3){fprintf(stderr,"Usage: loader-paths-probe --wine|--native <bundle-path>\n");return 2;}
    bool wine=!wcscmp(argv[1],L"--wine");str bundle=argv[2];
    const wchar_t *names[]={L"ddraw.dll",L"d3d8.dll",L"d3d9.dll",L"dinput.dll",L"dinput8.dll",L"dsound.dll"};
    const char *symbols[]={"DirectDrawCreate","Direct3DCreate8","Direct3DCreate9","DirectInputCreateA","DirectInput8Create","DirectSoundCreate"};
    wchar_t system[32768];GetSystemDirectoryW(system,32768);
    str executable=module_path(nullptr),app=executable.substr(0,executable.find_last_of(L"\\/"));
    using LdrLoad=NTSTATUS(NTAPI*)(PCWSTR,PULONG,const UNICODE_STRING*,HMODULE*);
    auto ldr=(LdrLoad)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll");
#ifdef WINEDOWS_STATIC_LOADER_IMPORTS
    printf("Static import anchor: %p\n",imports[0]);
#endif
    for(unsigned index=0;index<6;index++){
        str name=names[index],expected=bundle+L"\\"+name,system_path=str(system)+L"\\"+name;
        printf("MODULE %ls\n",name.c_str());
        if(!wine&&GetFileAttributesW(system_path.c_str())==INVALID_FILE_ATTRIBUTES){
            HMODULE absent=LoadLibraryW(system_path.c_str());DWORD error=GetLastError();
            check(!absent&&(error==ERROR_MOD_NOT_FOUND||error==ERROR_FILE_NOT_FOUND),"absent native architecture returns missing-module error");
            printf("UNAVAILABLE native %ls\n",name.c_str());continue;
        }
        HMODULE base=LoadLibraryW(name.c_str());check(base!=nullptr,"ordinary basename load");if(!base)continue;
        printf("Provider: %ls\n",module_path(base).c_str());
        check(wine?equal(module_path(base),expected):!equal(module_path(base),expected),"provider selection");
#ifdef WINEDOWS_STATIC_LOADER_IMPORTS
        check(imported(narrow(name).c_str(),symbols[index])==GetProcAddress(base,symbols[index]),"static IAT matches selected provider");
#else
        (void)symbols;
#endif
        str missing=L"C:\\winedows-loader-path-definitely-missing\\"+name;
        str marker=app+L"\\resources\\"+name;
        str upper=name;for(auto &c:upper)c=(wchar_t)towupper(c);
        std::vector<str> paths={name,upper,name.substr(0,name.size()-4),system_path,missing,marker};
        for(const auto &path:paths){
            bool must_fail=!wine&&path==missing;
            for(bool ansi:{false,true}){
                SetLastError(0);HMODULE loaded=ansi?LoadLibraryA(narrow(path).c_str()):LoadLibraryW(path.c_str());DWORD error=GetLastError();
                if(must_fail){check(!loaded&&(error==ERROR_MOD_NOT_FOUND||error==ERROR_FILE_NOT_FOUND),"missing full path preserves native error");continue;}
                check(loaded!=nullptr,"ordinary/explicit/case/extensionless load");if(!loaded)continue;
                if(wine)check(loaded==base,"explicit path reuses selected Wine module");
                else if(path==marker)check(GetProcAddress(loaded,"LoaderPathMarker")!=nullptr,"native full path loads requested app provider");
                else check(loaded==base,"native ordinary/system path reuses native provider");
                if(wine||path!=marker){
                    check(GetModuleHandleW(path.c_str())==loaded,"GetModuleHandleW alias");
                    check(GetModuleHandleA(narrow(path).c_str())==loaded,"GetModuleHandleA alias");
                    HMODULE lookup=nullptr;
                    check(GetModuleHandleExW(0,path.c_str(),&lookup)&&lookup==loaded,"GetModuleHandleEx adds reference");
                    if(lookup)check(!!FreeLibrary(lookup),"balance handle lookup reference");
                    lookup=nullptr;
                    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,path.c_str(),&lookup)&&lookup==loaded,"unchanged-refcount handle lookup");
                }
                check(!!FreeLibrary(loaded),"balance explicit load reference");
            }
        }
        if(wine){
            for(DWORD flags:{LOAD_WITH_ALTERED_SEARCH_PATH,LOAD_LIBRARY_SEARCH_SYSTEM32,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS}){
                HMODULE loaded=LoadLibraryExW(system_path.c_str(),nullptr,flags);check(loaded==base,"LoadLibraryEx search flags preserve Wine selection");if(loaded)FreeLibrary(loaded);
            }
            UNICODE_STRING us{(USHORT)(missing.size()*2),(USHORT)(missing.size()*2),missing.data()};HMODULE loaded=nullptr;
            check(ldr&&ldr(nullptr,nullptr,&us,&loaded)>=0&&loaded==base,"direct LdrLoadDll honors Wine full-path override");if(loaded)FreeLibrary(loaded);
        }
        HMODULE from_address=nullptr;
        check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)base,&from_address)&&from_address==base,"address-based handle lookup unchanged");
        for(DWORD flags:{LOAD_LIBRARY_AS_DATAFILE,LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE,LOAD_LIBRARY_AS_IMAGE_RESOURCE,LOAD_LIBRARY_AS_IMAGE_RESOURCE|LOAD_LIBRARY_AS_DATAFILE}){
            resource(marker,flags,false);resource(marker,flags,true);
            HMODULE absent=LoadLibraryExW(missing.c_str(),nullptr,flags);check(!absent,"missing resource path stays missing");if(absent)FreeLibrary(absent);
        }
        check(!!FreeLibrary(base),"balance baseline load reference");
    }
    HMODULE unknown=LoadLibraryW(L"C:\\winedows-loader-path-definitely-missing\\unselected-provider.dll");
    check(!unknown&&GetLastError()==ERROR_MOD_NOT_FOUND,"unselected nonexistent DLL remains an error");if(unknown)FreeLibrary(unknown);
    printf("Loader paths failures: %u\n",failures);return failures?1:0;
}
