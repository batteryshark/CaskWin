// SPDX-License-Identifier: LGPL-2.1-or-later
// Native two-process experiment for mixed DPI child hosting. The only changed
// DPI/hosting context belongs to this diagnostic's parent creation thread.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
template<class T> static T symbol(HMODULE module,const char *name){T value{};auto address=GetProcAddress(module,name);static_assert(sizeof(value)==sizeof(address),"Windows export pointer size");memcpy(&value,&address,sizeof(value));return value;}
int wmain(int argc,wchar_t **argv){
    if(argc!=4)return 1;
    setvbuf(stdout,nullptr,_IONBF,0);bool pm=wcsstr(argv[1],L"pm")!=nullptr,mixed=wcsstr(argv[1],L"mixed")!=nullptr;
    auto user=GetModuleHandleW(L"user32.dll");
    auto set_context=symbol<decltype(&SetThreadDpiAwarenessContext)>(user,"SetThreadDpiAwarenessContext");
    auto set_hosting=symbol<decltype(&SetThreadDpiHostingBehavior)>(user,"SetThreadDpiHostingBehavior");
    auto get_hosting=symbol<decltype(&GetWindowDpiHostingBehavior)>(user,"GetWindowDpiHostingBehavior");
    if(!set_context||!set_hosting||!get_hosting)return 77;
    auto old_context=set_context(pm?DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:DPI_AWARENESS_CONTEXT_UNAWARE);
    if(!old_context)return 1;
    auto old_hosting=set_hosting(mixed?DPI_HOSTING_BEHAVIOR_MIXED:DPI_HOSTING_BEHAVIOR_DEFAULT);
    if(old_hosting==DPI_HOSTING_BEHAVIOR_INVALID){set_context(old_context);return 1;}
    HWND parent=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Owned mixed DPI parent",WS_POPUP,0,0,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    set_hosting(old_hosting);set_context(old_context);if(!parent)return 1;
    printf("{\"kind\":\"native-host\",\"requestedPerMonitorV2\":%s,\"requestedMixed\":%s,\"actualHosting\":%d}\n",pm?"true":"false",mixed?"true":"false",int(get_hosting(parent)));
    ShowWindow(parent,SW_SHOWNOACTIVATE);
    std::wstring command=L"\""+std::wstring(argv[2])+L"\" "+argv[3]+L" hosted hidden-popup "+std::to_wstring(reinterpret_cast<UINT_PTR>(parent));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);PROCESS_INFORMATION process{};
    if(!CreateProcessW(argv[2],command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){DestroyWindow(parent);return 1;}
    DWORD deadline=GetTickCount()+10000,code=99;
    while((LONG)(deadline-GetTickCount())>0){
        if(MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,50,QS_ALLINPUT)==WAIT_OBJECT_0){GetExitCodeProcess(process.hProcess,&code);break;}
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(code==99){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,2000);}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);DestroyWindow(parent);
    printf("{\"kind\":\"native-host-summary\",\"childExitCode\":%lu}\n",code);return int(code);
}
