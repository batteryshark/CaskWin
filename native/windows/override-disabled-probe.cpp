// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#if defined(OVERRIDE_MARKER_DLL)
extern "C" __declspec(dllexport) DWORD MarkerValue(){return 0x4e415449;}
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,void*){
    if(reason==DLL_PROCESS_ATTACH){wchar_t path[32768]{};DWORD n=GetEnvironmentVariableW(L"WINEDOWS_OVERRIDE_MARKER",path,32768);
        if(n&&n<32768){HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);if(file!=INVALID_HANDLE_VALUE){DWORD value=MarkerValue(),written=0;WriteFile(file,&value,sizeof(value),&written,nullptr);CloseHandle(file);}}}
    return TRUE;
}
#elif defined(OVERRIDE_BRIDGE_DLL)
extern "C" __declspec(dllimport) DWORD MarkerValue();
extern "C" __declspec(dllexport) DWORD WINAPI BridgeValue(){return MarkerValue();}
BOOL WINAPI DllMain(HINSTANCE,DWORD,void*){return TRUE;}
#else
using text=std::wstring;
static int failures;
static void check(bool value,const char *label){printf("%s %s\n",value?"PASS":"FAIL",label);if(!value)++failures;}
static text executable(){wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,32768);return text(path,n);}
static text full_library(const text &name){
    text path=name;if(path.find_first_of(L"\\/:")==path.npos){path=executable();path.resize(path.find_last_of(L"\\/")+1);path+=name;}
    auto leaf=path.substr(path.find_last_of(L"\\/")+1);if(leaf.find(L'.')==leaf.npos)path+=L".dll";return path;
}
static void blocked(const text &name,bool direct=true){
    using Load=NTSTATUS(NTAPI*)(PCWSTR,PULONG,const UNICODE_STRING*,HMODULE*);
    auto load=(Load)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll");
    for(const auto &request:{name,full_library(name)}){
        SetLastError(0);HMODULE module=LoadLibraryW(request.c_str());DWORD error=GetLastError();
        printf("LOAD request=%ls error=%lu\n",request.c_str(),error);check(!module&&error==ERROR_MOD_NOT_FOUND,"disabled Win32 executable load reports module not found");if(module)FreeLibrary(module);
        std::string ansi(request.begin(),request.end());SetLastError(0);module=LoadLibraryExA(ansi.c_str(),nullptr,0);error=GetLastError();
        check(!module&&error==ERROR_MOD_NOT_FOUND,"disabled ANSI load reports module not found");if(module)FreeLibrary(module);
        module=LoadLibraryExW(request.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);error=GetLastError();
        if(direct)check(!module&&error==ERROR_MOD_NOT_FOUND,"disabled no-resolve executable image cannot bypass override");
        else check(module!=nullptr,"no-resolve parent load does not require its disabled dependency");if(module)FreeLibrary(module);
        UNICODE_STRING unicode{(USHORT)(request.size()*2),(USHORT)(request.size()*2),const_cast<wchar_t*>(request.data())};module=(HMODULE)(ULONG_PTR)0x1234;
        NTSTATUS status=load(nullptr,nullptr,&unicode,&module);printf("LDR status=%08lx output=%p\n",(ULONG)status,module);
        // A direct policy rejection clears its output. Native Windows leaves
        // the caller's output untouched when a parent's dependency fails; keep
        // that measured native ABI for the indirect-load path.
        check(status==(NTSTATUS)0xc0000135&&(direct?!module:module==(HMODULE)(ULONG_PTR)0x1234),direct?"disabled native loader reports missing module and null output":"transitive native failure preserves the measured output sentinel");if(status>=0&&module)FreeLibrary(module);
    }
}
static void resources(const text &path){
    for(DWORD flags:{LOAD_LIBRARY_AS_DATAFILE,LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE,LOAD_LIBRARY_AS_IMAGE_RESOURCE}){
        HMODULE module=LoadLibraryExW(path.c_str(),nullptr,flags);check(module!=nullptr,"disabled DLL remains available as a resource image");
        if(module){HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(101),RT_RCDATA);HGLOBAL loaded=resource?LoadResource(module,resource):nullptr;void *data=loaded?LockResource(loaded):nullptr;
            check(data&&SizeofResource(module,resource)==4&&*(DWORD*)data==0x56781234,"resource bytes are read without executing DLL entrypoint");FreeLibrary(module);}
    }
}
static void native(const text &name){
    HMODULE module=LoadLibraryW(name.c_str());check(module!=nullptr,"native provider loads");if(!module)return;
    using Marker=DWORD(*)();auto marker=(Marker)GetProcAddress(module,"MarkerValue");check(marker&&marker()==0x4e415449,"actual native fixture implementation executes");
    wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);printf("NATIVE file=%ls\n",path);FreeLibrary(module);
}
static void builtin(const text &name){
    HMODULE module=LoadLibraryW(name.c_str());check(module!=nullptr,"Wine provider loads");if(!module)return;
    wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);text actual=path;std::transform(actual.begin(),actual.end(),actual.begin(),towlower);
    printf("BUILTIN file=%ls\n",path);check(actual.find(sizeof(void*)==8?L"\\builtin\\x64\\":L"\\builtin\\x86\\")!=actual.npos,"selected module comes from real Wine bundle");
    check(GetProcAddress(module,"wined3d_create")!=nullptr&&!GetProcAddress(module,"MarkerValue"),"real Wine implementation exports are bound");FreeLibrary(module);
}
static void existing(){
    HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");using Backend=HMODULE(WINAPI*)();
    auto backend=shim?(Backend)GetProcAddress(shim,"WinedowsLoadSystemXInput"):nullptr;check(backend!=nullptr,"trusted host backend loader is present");if(!backend)return;
    HMODULE host=backend();check(host!=nullptr,"fixed native backend loads despite application override");if(!host)return;
    check(GetProcAddress(host,"XInputGetState")&&!GetProcAddress(host,"MarkerValue"),"backend is real system XInput despite app-local shadow");
    check(GetModuleHandleW(L"xinput1_4.dll")==host,"disabled policy does not hide an already loaded backend");
    HMODULE acquired=LoadLibraryW(L"xinput1_4.dll");check(acquired==host,"disabled policy reuses an existing module like Wine");
    if(acquired)FreeLibrary(acquired);check(GetModuleHandleW(L"xinput1_4.dll")==host,"ordinary load/free preserves backend reference");FreeLibrary(host);
}
static int child(const text &path,const text &policy){
    bool duplicate=policy==L"duplicate-disabled"||policy==L"duplicate-native";
    bool enable=policy==L"clear"||policy==L"native"||policy==L"duplicate-native",reject=policy==L"startup";
    text line=L"\""+path+L"\" "+(enable?L"--native":L"--blocked")+L" override-marker.dll";
    if(duplicate)line=L"\""+path+L"\" --environment C:\\WinedowsFirstPath "+(enable?L"override-marker=n":L"override-marker=")+L" "+(enable?L"native":L"blocked");
    std::vector<wchar_t> block;
    if(policy!=L"inherit"&&policy!=L"startup"){
        std::vector<text> values;
        for(auto variable:{L"SystemRoot",L"PATH",L"TEMP",L"TMP",L"WINEDOWS_OVERRIDE_MARKER"}){if(duplicate&&!wcscmp(variable,L"PATH"))continue;wchar_t value[32768]{};DWORD size=GetEnvironmentVariableW(variable,value,32768);if(size&&size<32768)values.push_back(text(variable)+L"="+value);}
        if(policy==L"clear")values.push_back(L"WINEDLLOVERRIDES=");
        if(policy==L"native")values.push_back(L"WINEDLLOVERRIDES=override-marker=n");
        if(duplicate){
            values.push_back(enable?L"wInEdLlOvErRiDeS=override-marker=n":L"wInEdLlOvErRiDeS=override-marker=");
            values.push_back(enable?L"WINEDLLOVERRIDES=override-marker=":L"WINEDLLOVERRIDES=override-marker=n");
            values.push_back(L"pAtH=C:\\WinedowsFirstPath");values.push_back(L"PATH=C:\\WinedowsSecondPath");
        }else std::sort(values.begin(),values.end());
        for(const auto &value:values){block.insert(block.end(),value.begin(),value.end());block.push_back(0);}block.push_back(0);
    }
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};SetLastError(0);
    BOOL ok=CreateProcessW(path.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,block.empty()?nullptr:block.data(),nullptr,&startup,&process);DWORD error=GetLastError();
    printf("CHILD policy=%ls created=%d error=%lu\n",policy.c_str(),ok,error);
    if(reject){check(!ok&&(error==ERROR_MOD_NOT_FOUND||error==ERROR_DLL_INIT_FAILED)&&!process.hProcess&&!process.hThread,"disabled startup import rejects child before execution");if(!ok)return failures?1:0;}
    else check(ok!=FALSE,"child starts with resolved per-child override policy");
    if(ok){DWORD waited=WaitForSingleObject(process.hProcess,30000);if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,124);DWORD code=1;GetExitCodeProcess(process.hProcess,&code);check(waited==WAIT_OBJECT_0&&code==0,"child validates actual provider behavior");CloseHandle(process.hThread);CloseHandle(process.hProcess);}
    return failures?1:0;
}
#ifdef OVERRIDE_STATIC_IMPORT
extern "C" __declspec(dllimport) DWORD MarkerValue();
#endif
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#ifdef OVERRIDE_STATIC_IMPORT
    check(MarkerValue()==0x4e415449,"static imported marker executes");
#endif
    if(argc>=3&&!wcscmp(argv[1],L"--blocked"))blocked(argv[2]);
    else if(argc>=3&&!wcscmp(argv[1],L"--blocked-dependency"))blocked(argv[2],false);
    else if(argc>=3&&!wcscmp(argv[1],L"--resource"))resources(full_library(argv[2]));
    else if(argc>=3&&!wcscmp(argv[1],L"--native"))native(argv[2]);
    else if(argc>=3&&!wcscmp(argv[1],L"--builtin"))builtin(argv[2]);
    else if(argc>=2&&!wcscmp(argv[1],L"--existing"))existing();
    else if(argc==4&&!wcscmp(argv[1],L"--child"))return child(argv[2],argv[3]);
    else if(argc==5&&!wcscmp(argv[1],L"--environment")){
        wchar_t value[32768]{};GetEnvironmentVariableW(L"PATH",value,32768);check(text(value)==argv[2],"mixed-case duplicate PATH retains first native environment value");
        GetEnvironmentVariableW(L"WINEDLLOVERRIDES",value,32768);check(text(value)==argv[3],"mixed-case duplicate overrides retain first native environment value");
        if(!wcscmp(argv[4],L"native"))native(L"override-marker.dll");else blocked(L"override-marker.dll");
    }
    else if(argc>=2&&!wcscmp(argv[1],L"--noop")){}
    else return 2;
    printf("Disabled override %zu-bit: %d failures\n",sizeof(void*)*8,failures);return failures?1:0;
}
#endif
