// SPDX-License-Identifier: LGPL-2.1-or-later
// Fixed host-loader boundary only. No Vulkan instance, device, or GPU work.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <string>
#include <vector>

#ifdef WINEDOWS_VULKAN_LOADER_MARKER
extern "C" __declspec(dllexport) DWORD WINAPI WinedowsVulkanLoaderMarker(){return 0x564b4944;}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
#else
static int failures;
static void check(bool value,const char *message,DWORD error=0){printf("%s %s error=%lu\n",value?"PASS":"FAIL",message,error);if(!value)++failures;}
struct Identity {ULONGLONG volume=0;BYTE file[16]{};};
static bool same(const Identity &a,const Identity &b){return a.volume==b.volume&&!memcmp(a.file,b.file,sizeof(a.file));}
static void print_identity(const char *label,const Identity &id){printf("%s %llx ",label,id.volume);for(BYTE part:id.file)printf("%02x",part);putchar('\n');}
static bool parse_identity(const wchar_t *volume,const wchar_t *file,Identity &id){
    wchar_t *end=nullptr;id.volume=wcstoull(volume,&end,16);if(!volume[0]||*end||wcslen(file)!=32)return false;
    for(unsigned i=0;i<16;i++){wchar_t part[]={file[i*2],file[i*2+1],0};unsigned long value=wcstoul(part,&end,16);if(end!=part+2||value>255)return false;id.file[i]=BYTE(value);}return true;
}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));return n&&n<path.size()?std::wstring(path.data(),n):std::wstring();}
static std::wstring system_path(){
    wchar_t path[MAX_PATH]{};UINT n=sizeof(void*)==4?GetSystemWow64DirectoryW(path,MAX_PATH):0;
    if(!n)n=GetSystemDirectoryW(path,MAX_PATH);
    return n&&n<MAX_PATH&&path[0]?std::wstring(path,n)+L"\\vulkan-1.dll":std::wstring();
}
static bool image_identity(HMODULE module,Identity &id){
    // The mapped image's native device path avoids DOS-path prefix translation.
    // Compare with the identity measured by the separate unhooked baseline.
    std::vector<wchar_t> path(32768);DWORD n=GetMappedFileNameW(GetCurrentProcess(),module,path.data(),DWORD(path.size()));
    if(!n||n>=path.size()||path[0]!=L'\\')return false;
    std::wstring native=L"\\\\?\\GLOBALROOT"+std::wstring(path.data(),n);
    HANDLE file=CreateFileW(native.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    // FileIdInfo returns the volume/file identity together, avoiding the separate
    // volume-information request that the prefix intentionally virtualizes.
    FILE_ID_INFO info{};BOOL ok=GetFileInformationByHandleEx(file,FileIdInfo,&info,sizeof(info));CloseHandle(file);
    if(ok){id.volume=info.VolumeSerialNumber;memcpy(id.file,info.FileId.Identifier,sizeof(id.file));}
    wprintf(L"MAPPED_IMAGE %ls\n",native.c_str());return ok!=FALSE;
}
static bool marker(HMODULE module){auto fn=(DWORD(WINAPI*)())GetProcAddress(module,"WinedowsVulkanLoaderMarker");return fn&&fn()==0x564b4944;}
static void loader_exports(HMODULE module){
    using Gipa=FARPROC(WINAPI*)(void*,const char*);
    auto gipa=(Gipa)GetProcAddress(module,"vkGetInstanceProcAddr");
    check(gipa&&GetProcAddress(module,"vkCreateInstance")&&GetProcAddress(module,"vkEnumerateInstanceExtensionProperties"),"real Vulkan loader exposes required global entry points");
    if(!gipa)return;
    check(gipa(nullptr,"vkCreateInstance")!=nullptr,"actual host dispatch resolves vkCreateInstance without creating an instance");
    check(gipa(nullptr,"vkWinedowsDefinitelyMissingFunction")==nullptr,"unknown Vulkan entry point is absent");
    auto version=(int32_t(WINAPI*)(uint32_t*))gipa(nullptr,"vkEnumerateInstanceVersion");
    if(version){uint32_t value=0;int32_t result=version(&value);printf("HOST_VULKAN_VERSION %u status=%d\n",value,result);check(result==0&&value>=(1u<<22),"host loader version query succeeds without GPU creation");}
    else puts("INFO Vulkan1.0 loader has no optional instance-version export");
}
static HMODULE ordinary_shadow(){
    HMODULE module=LoadLibraryW(L"vulkan-1.dll");DWORD error=GetLastError();
    check(module&&marker(module),"ordinary application lookup still selects its private marker",error);
    if(module){Identity id{};check(image_identity(module,id),"private marker has a resolvable image identity");check(!GetProcAddress(module,"vkGetInstanceProcAddr"),"marker deliberately lacks Vulkan dispatch exports");}
    return module;
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc<2)return 2;
    std::wstring mode=argv[1];
    if(mode==L"--native"||mode==L"--raw-local"){
        std::wstring path=system_path();check(!path.empty(),"resolve architecture-specific native system path");
        HMODULE module=path.empty()?nullptr:LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);DWORD error=GetLastError();
        if(!module&&mode==L"--native"&&(error==ERROR_MOD_NOT_FOUND||error==ERROR_FILE_NOT_FOUND)){puts("UNAVAILABLE host Vulkan loader");return 77;}
        check(module!=nullptr,"native fixed-path Vulkan load",error);if(!module)return 1;
        if(mode==L"--raw-local"){
            bool redirected=marker(module);FreeLibrary(module);
            if(!redirected){puts("UNAVAILABLE native DotLocal policy ignored this fixture; host policy unchanged");return 77;}
            puts("PASS native DotLocal resolves fixed system path to benign marker");return 0;
        }
        Identity id{};check(image_identity(module,id),"identify actual native Vulkan image");
        print_identity("HOST_ID",id);wprintf(L"HOST_PATH %ls\n",module_path(module).c_str());
        check(!marker(module),"native baseline is not a fixture marker");loader_exports(module);FreeLibrary(module);return failures?1:0;
    }
    HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
    auto backend=shim?(HMODULE(WINAPI*)())GetProcAddress(shim,"WinedowsLoadSystemVulkan"):nullptr;
    check(backend!=nullptr,"fixed no-argument Vulkan backend export exists");if(!backend)return 1;
    if(mode==L"--reject-local"){
        auto executable=module_path(nullptr);auto local=executable.substr(0,executable.find_last_of(L"\\/"))+L"\\vulkan-1.dll";
        for(unsigned i=0;i<3;i++){
            SetLastError(4321);HMODULE module=backend();DWORD error=GetLastError();
            check(!module&&error==ERROR_INVALID_DLL,"redirected fixed backend is rejected with ERROR_INVALID_DLL",error);
            if(module)FreeLibrary(module);
            check(!GetModuleHandleW(local.c_str()),"rejected marker reference is released");
        }return failures?1:0;
    }
    if(mode!=L"--accept"||argc!=5)return 2;
    Identity expected{};if(!parse_identity(argv[2],argv[3],expected))return 2;
    print_identity("EXPECTED_ID",expected);
    bool shadow=!wcscmp(argv[4],L"marker");if(!shadow&&wcscmp(argv[4],L"clean"))return 2;
    HMODULE held=shadow?ordinary_shadow():nullptr;
    for(unsigned i=0;i<3;i++){
        HMODULE host=backend();DWORD error=GetLastError();check(host!=nullptr,"load actual fixed Vulkan host backend",error);
        if(host){Identity actual{};bool identified=image_identity(host,actual);print_identity("ACTUAL_ID",actual);check(identified&&same(expected,actual),"host backend file identity matches independent native baseline");check(!marker(host),"private marker cannot replace host Vulkan backend");loader_exports(host);FreeLibrary(host);}
        if(held)check(marker(held),"backend load/free preserves the already selected private provider reference");
    }
    if(held)FreeLibrary(held);
    if(shadow){HMODULE after=ordinary_shadow();if(after)FreeLibrary(after);}
    printf("Vulkan fixed loader %zu-bit failures=%d\n",sizeof(void*)*8,failures);return failures?1:0;
}
#endif
