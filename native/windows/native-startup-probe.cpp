// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#ifdef WINEDOWS_BRIDGE_IMPORT
extern "C" __declspec(dllimport) int WinedowsBridgeMarker();
static int imported_marker(){return WinedowsBridgeMarker();}
#else
extern "C" __declspec(dllimport) int WinedowsNativeMarker();
static int imported_marker(){return WinedowsNativeMarker();}
#endif
static unsigned failures;
static void check(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,ok?0:GetLastError());if(!ok)++failures;}
static bool same_file(const wchar_t *left,const wchar_t *right){
    HANDLE a=CreateFileW(left,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(right,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    bool same=a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y)&&
        x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);return same;
}
static DWORD WINAPI watchdog(void*){Sleep(45000);TerminateProcess(GetCurrentProcess(),124);return 0;}
static std::wstring quote(const wchar_t *text){return L"\""+std::wstring(text)+L"\"";}
static std::string ansi(const std::wstring &text){int size=WideCharToMultiByte(CP_ACP,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);std::string result(size,0);WideCharToMultiByte(CP_ACP,0,text.c_str(),-1,result.data(),size,nullptr,nullptr);return result;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    if(argc<4){fputs("native-startup-probe EXPECTED_HEX FILE native|builtin [--child PROGRAM EXPECTED_HEX FILE] [--custom-environment] [--suspended] [--dll-directory DIR] [--child-exit HEX]\n",stderr);return 2;}
    wchar_t environment[8192]{};
    check(GetEnvironmentVariableW(L"WD_STARTUP_SENTINEL",environment,8192)&&!wcscmp(environment,L"prefix-env-preserved"),"startup keeps inherited environment values intact");
    if(GetEnvironmentVariableW(L"WD_STARTUP_LONG",environment,8192)){
        bool intact=wcslen(environment)==4097;for(unsigned i=0;intact&&i<4097;i++)intact=environment[i]==L'x';
        check(intact,"startup keeps a long environment value intact");
    }
    int expected=(int)wcstoul(argv[1],nullptr,16);
    check(imported_marker()==expected,"named startup import executes the selected initialized provider");
#ifdef WINEDOWS_SHADOW_IMPORT
    const wchar_t *module_name=L"d3dcompiler_43.dll";
#else
    const wchar_t *module_name=L"wined3d.dll";
#endif
    HMODULE module=nullptr;
#if defined(WINEDOWS_STARTUP_IAT_IDENTITY) && !defined(WINEDOWS_BRIDGE_IMPORT)
    // A manifest can redirect a basename query even though an explicitly
    // selected full-path provider owns the actual startup IAT. Measure the
    // dllimport pointer, not a second independent loader name resolution.
    HMODULE owner=nullptr;
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&WinedowsNativeMarker,&owner)!=FALSE,"startup IAT owner is already loaded");
    module=GetModuleHandleW(argv[2]);check(module!=nullptr,"full-path selected provider is loaded");
    // A genuine export forwarder can give the final IAT address a different
    // owner. The selected file must still resolve its exact export to that
    // address; comparing only basename handles would miss SxS substitutions.
    check(module&&GetProcAddress(module,"WinedowsNativeMarker")==reinterpret_cast<FARPROC>(&WinedowsNativeMarker),"selected provider export agrees with actual startup IAT");
    wchar_t owner_path[32768]{};if(owner)GetModuleFileNameW(owner,owner_path,32768);wprintf(L"Actual IAT owner: %ls\n",owner_path);
    if(GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")){
        check(GetModuleHandleW(module_name)==module,"prefixed basename module query agrees with actual startup IAT");
        HMODULE loaded=LoadLibraryW(module_name);check(loaded==module,"prefixed dynamic basename load agrees with actual startup IAT");if(loaded)FreeLibrary(loaded);
    }
#else
    module=GetModuleHandleW(module_name);check(module!=nullptr,"startup provider is already loaded");
#endif
    wchar_t path[32768]{};if(module)GetModuleFileNameW(module,path,32768);
    wprintf(L"Startup provider: %ls\n",path);
    check(module&&same_file(path,argv[2]),"startup provider file identity matches selection");
    if(module){
        bool builtin=!wcscmp(argv[3],L"builtin");
        check((GetProcAddress(module,MAKEINTRESOURCEA(1))!=nullptr)==builtin,"native startup provider does not need ordinal 1");
        check(GetProcAddress(module,"WinedowsNativeMarker")!=nullptr,"named import export is present");
    }
    const wchar_t *child=nullptr,*child_value=nullptr,*child_file=nullptr,*dll_directory=nullptr,*child_kind=L"native";
    bool custom=false,suspended=false,require_custom=false,ansi_child=false,system_child=false;DWORD expected_exit=0;
    for(int i=4;i<argc;i++){
        if(!wcscmp(argv[i],L"--child")&&i+3<argc){child=argv[++i];child_value=argv[++i];child_file=argv[++i];}
        else if(!wcscmp(argv[i],L"--custom-environment"))custom=true;
        else if(!wcscmp(argv[i],L"--require-custom"))require_custom=true;
        else if(!wcscmp(argv[i],L"--ansi-child"))ansi_child=true;
        else if(!wcscmp(argv[i],L"--system-child"))system_child=true;
        else if(!wcscmp(argv[i],L"--suspended"))suspended=true;
        else if(!wcscmp(argv[i],L"--dll-directory")&&i+1<argc)dll_directory=argv[++i];
        else if(!wcscmp(argv[i],L"--child-exit")&&i+1<argc)expected_exit=wcstoul(argv[++i],nullptr,16);
        else if(!wcscmp(argv[i],L"--child-kind")&&i+1<argc)child_kind=argv[++i];
        else return 2;
    }
    if(require_custom)check(GetEnvironmentVariableW(L"WD_STARTUP_CUSTOM",environment,8192)&&!wcscmp(environment,L"custom"),"caller-supplied custom environment reaches child intact");
    if(child||system_child){
        if(dll_directory)check(SetDllDirectoryW(dll_directory)!=FALSE,"application sets its inherited DLL directory");
        std::wstring command=system_child?L"cmd.exe /c exit 0":quote(child)+L" "+child_value+L" "+quote(child_file)+L" "+child_kind;
        if(custom)command+=L" --require-custom";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        wchar_t custom_environment[]=L"WD_STARTUP_SENTINEL=prefix-env-preserved\0WD_STARTUP_CUSTOM=custom\0\0";
        char custom_ansi_environment[]="WD_STARTUP_SENTINEL=prefix-env-preserved\0WD_STARTUP_CUSTOM=custom\0\0";
        DWORD flags=(custom&&!ansi_child?CREATE_UNICODE_ENVIRONMENT:0)|(suspended?CREATE_SUSPENDED:0);
        BOOL created;
        if(ansi_child){
            auto app_a=ansi(child?child:L""),command_a=ansi(command);STARTUPINFOA startup_a{};startup_a.cb=sizeof(startup_a);
            created=CreateProcessA(system_child?nullptr:app_a.c_str(),command_a.data(),nullptr,nullptr,FALSE,flags,custom?custom_ansi_environment:nullptr,nullptr,&startup_a,&process);
        }else created=CreateProcessW(system_child?nullptr:child,command.data(),nullptr,nullptr,FALSE,flags,custom?custom_environment:nullptr,nullptr,&startup,&process);
        check(created!=FALSE,"create static-import child");
        if(created){
            if(suspended){check(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT,"requested suspended child has not exited");check(ResumeThread(process.hThread)==1,"caller retains the child suspension count");}
            DWORD wait=WaitForSingleObject(process.hProcess,30000),code=124;
            if(wait==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else TerminateProcess(process.hProcess,124);
            printf("Child exit: %08lx; expected: %08lx\n",code,expected_exit);
            check(wait==WAIT_OBJECT_0&&code==expected_exit,expected_exit?"unsupported inherited-directory fallback remains explicit":"static-import child binds its architecture's selected native provider");
            CloseHandle(process.hThread);CloseHandle(process.hProcess);
        }
    }
    printf("Native startup %zu-bit failures: %u\n",sizeof(void*)*8,failures);return failures?1:0;
}
