// SPDX-License-Identifier: LGPL-2.1-or-later
// Optimized real Win32/NT IPC fixture. No device input or system settings change.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
static int failures;
template<class T>static T function(HMODULE module,const char *name){auto address=GetProcAddress(module,name);T result=nullptr;static_assert(sizeof(result)==sizeof(address));memcpy(&result,&address,sizeof(result));return result;}
static void check(bool ok,const char *name){printf("%s %s error=%lu\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static std::wstring object_name(const std::wstring &base,const wchar_t *suffix,bool global=false){return std::wstring(global?L"Global\\":L"Local\\")+base+suffix;}
static std::wstring nt_name(HANDLE handle){
    using Query=NTSTATUS(NTAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);auto query=function<Query>(GetModuleHandleW(L"ntdll.dll"),"NtQueryObject");
    std::vector<BYTE> data(65536);ULONG size=0;if(query(handle,1,data.data(),(ULONG)data.size(),&size)<0)return {};auto name=(UNICODE_STRING*)data.data();return std::wstring(name->Buffer,name->Length/2);
}
static void marker(const std::wstring &path){HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);check(h!=INVALID_HANDLE_VALUE,"fixture readiness marker");if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
static bool exists(const std::wstring &path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
struct Objects {
    HANDLE mutex{},event{},sem{},timer{},mapping{},global{};DWORD *page{};
    ~Objects(){if(page)UnmapViewOfFile(page);for(HANDLE h:{mutex,event,sem,timer,mapping,global})if(h)CloseHandle(h);}
    bool create(const std::wstring &base,DWORD value,bool private_expected){
        mutex=CreateMutexW(nullptr,FALSE,object_name(base,L".mutex").c_str());check(mutex&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh mutex namespace");
        event=CreateEventW(nullptr,TRUE,FALSE,object_name(base,L".event").c_str());check(event&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh event namespace");
        sem=CreateSemaphoreW(nullptr,1,4,object_name(base,L".semaphore").c_str());check(sem&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh semaphore namespace");
        timer=CreateWaitableTimerW(nullptr,TRUE,object_name(base,L".timer").c_str());check(timer&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh waitable timer namespace");
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,object_name(base,L".mapping").c_str());check(mapping&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh named section namespace");
        global=CreateMutexW(nullptr,FALSE,object_name(base,L".global",true).c_str());check(global&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh global mutex namespace");
        if(!mutex||!event||!sem||!timer||!mapping||!global)return false;
        page=(DWORD*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,4096);check(page!=nullptr,"map named shared section");if(!page)return false;*page=value;
        for(HANDLE h:{mutex,event,sem,timer,mapping,global}){
            auto name=nt_name(h);printf("OBJECT %ls\n",name.c_str());check(!name.empty()&&(name.find(L"Winedows.Objects.")!=name.npos)==private_expected,"physical object namespace matches origin");
        }
        HANDLE precise=CreateWaitableTimerExW(nullptr,object_name(base,L".precise").c_str(),2,TIMER_ALL_ACCESS);DWORD precise_status=precise?0:GetLastError();
        printf("TIMER_EX_STATUS %lu\n",precise_status);
        if(precise){check((nt_name(precise).find(L"Winedows.Objects.")!=std::wstring::npos)==private_expected,"high-resolution timer namespace matches origin");CloseHandle(precise);}
        else if(precise_status==ERROR_INVALID_PARAMETER)puts("UNAVAILABLE named high-resolution timer; runner compares native baseline");
        else check(false,"high-resolution timer has supported or native-rejected status");
        return true;
    }
};
static void absent_host(const std::wstring &base){
    HANDLE h=OpenMutexW(SYNCHRONIZE,FALSE,object_name(base,L".mutex").c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host mutex cannot satisfy private open");if(h)CloseHandle(h);
    h=OpenEventW(SYNCHRONIZE,FALSE,object_name(base,L".event").c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host event cannot satisfy private open");if(h)CloseHandle(h);
    h=OpenSemaphoreW(SYNCHRONIZE,FALSE,object_name(base,L".semaphore").c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host semaphore cannot satisfy private open");if(h)CloseHandle(h);
    h=OpenWaitableTimerW(SYNCHRONIZE,FALSE,object_name(base,L".timer").c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host timer cannot satisfy private open");if(h)CloseHandle(h);
    h=OpenFileMappingW(FILE_MAP_READ,FALSE,object_name(base,L".mapping").c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host section cannot satisfy private open");if(h)CloseHandle(h);
    h=OpenMutexW(SYNCHRONIZE,FALSE,object_name(base,L".global",true).c_str());check(!h&&GetLastError()==ERROR_FILE_NOT_FOUND,"host global mutex cannot satisfy private open");if(h)CloseHandle(h);
}
static void raw_roundtrip(HANDLE event,const std::wstring &base){
    using Open=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);auto nt=GetModuleHandleW(L"ntdll.dll");
    auto open_event=function<Open>(nt,"NtOpenEvent"),open_directory=function<Open>(nt,"NtOpenDirectoryObject");
    auto physical=nt_name(event);UNICODE_STRING name{(USHORT)(physical.size()*2),(USHORT)(physical.size()*2+2),physical.data()};OBJECT_ATTRIBUTES a{};a.Length=sizeof(a);a.ObjectName=&name;
    HANDLE opened=nullptr;check(open_event(&opened,EVENT_MODIFY_STATE|SYNCHRONIZE,&a)>=0,"raw NT physical query-name round trip avoids double mapping");
    if(opened){SetEvent(opened);check(WaitForSingleObject(event,0)==WAIT_OBJECT_0,"physical-name reopened handle is same event");ResetEvent(event);CloseHandle(opened);}
    auto directory=physical.substr(0,physical.find_last_of(L'\\'));name={(USHORT)(directory.size()*2),(USHORT)(directory.size()*2+2),directory.data()};HANDLE root=nullptr;
    check(open_directory(&root,3,&a)>=0,"open explicit native object directory");
    if(root){auto logical=base+L".event";name={(USHORT)(logical.size()*2),(USHORT)(logical.size()*2+2),logical.data()};a.RootDirectory=root;opened=nullptr;
        check(open_event(&opened,EVENT_MODIFY_STATE|SYNCHRONIZE,&a)>=0,"raw NT logical name with explicit RootDirectory shares private object");
        if(opened){SetEvent(opened);check(WaitForSingleObject(event,0)==WAIT_OBJECT_0,"root-relative handle is same event");ResetEvent(event);CloseHandle(opened);}CloseHandle(root);}
}
static void local_contracts(Objects &objects,const std::wstring &base){
    HANDLE again=CreateMutexExW(nullptr,object_name(base,L".mutex").c_str(),0,MUTEX_ALL_ACCESS);check(again&&GetLastError()==ERROR_ALREADY_EXISTS,"CreateMutexEx preserves already-exists status");if(again)CloseHandle(again);
    again=CreateEventW(nullptr,FALSE,FALSE,object_name(base,L".mutex").c_str());check(!again&&GetLastError()==ERROR_INVALID_HANDLE,"different object types share one collision namespace");if(again)CloseHandle(again);
    HANDLE duplicate=nullptr;check(DuplicateHandle(GetCurrentProcess(),objects.event,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS),"duplicate private event handle");
    if(duplicate){SetEvent(duplicate);check(WaitForSingleObject(objects.event,0)==WAIT_OBJECT_0,"duplicate preserves event identity");ResetEvent(objects.event);CloseHandle(duplicate);}
    HANDLE unnamed1=CreateEventW(nullptr,TRUE,FALSE,nullptr),unnamed2=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    check(unnamed1&&unnamed2&&nt_name(unnamed1).empty(),"unnamed objects remain unnamed");if(unnamed1&&unnamed2){SetEvent(unnamed1);check(WaitForSingleObject(unnamed2,0)==WAIT_TIMEOUT,"unnamed objects remain distinct");}if(unnamed1)CloseHandle(unnamed1);if(unnamed2)CloseHandle(unnamed2);
    auto plain=base+L".alias";HANDLE local=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\"+plain).c_str());again=CreateEventW(nullptr,TRUE,FALSE,plain.c_str());check(local&&again&&GetLastError()==ERROR_ALREADY_EXISTS,"Local and unqualified aliases resolve to same session object");if(local)CloseHandle(local);if(again)CloseHandle(again);
    HANDLE upper=CreateEventW(nullptr,TRUE,FALSE,object_name(base,L".Case").c_str()),lower=CreateEventW(nullptr,TRUE,FALSE,object_name(base,L".case").c_str());check(upper&&lower&&GetLastError()!=ERROR_ALREADY_EXISTS,"application object names remain case-sensitive");if(upper)CloseHandle(upper);if(lower)CloseHandle(lower);
    LARGE_INTEGER due{};due.QuadPart=-10000;check(SetWaitableTimer(objects.timer,&due,0,nullptr,nullptr,FALSE)&&WaitForSingleObject(objects.timer,2000)==WAIT_OBJECT_0,"named waitable timer fires normally");
    raw_roundtrip(objects.event,base);
}
static int child(const std::wstring &base,DWORD expected){
    HANDLE event=OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,object_name(base,L".event").c_str()),mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,object_name(base,L".mapping").c_str());
    check(event&&mapping,"same-prefix child opens event and section");DWORD *page=mapping?(DWORD*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,4096):nullptr;
    check(page&&*page==expected,"cross-bitness child sees prefix-specific shared memory");if(page){*page=expected+1;UnmapViewOfFile(page);}if(event){SetEvent(event);CloseHandle(event);}if(mapping)CloseHandle(mapping);
    for(auto suffix:{L".mutex",L".global"}){HANDLE h=OpenMutexW(SYNCHRONIZE,FALSE,object_name(base,suffix,!wcscmp(suffix,L".global")).c_str());check(h!=nullptr,"child opens local/global mutex in same prefix");if(h)CloseHandle(h);}
    HANDLE sem=OpenSemaphoreW(SYNCHRONIZE|SEMAPHORE_MODIFY_STATE,FALSE,object_name(base,L".semaphore").c_str());check(sem&&WaitForSingleObject(sem,0)==WAIT_OBJECT_0,"child acquires prefix semaphore");if(sem)CloseHandle(sem);
    HANDLE timer=OpenWaitableTimerW(SYNCHRONIZE,FALSE,object_name(base,L".timer").c_str());check(timer&&WaitForSingleObject(timer,0)==WAIT_OBJECT_0,"child opens signaled private timer");if(timer)CloseHandle(timer);
    return failures?1:0;
}
static DWORD WINAPI watchdog(void*){Sleep(90000);fputs("FAIL named object watchdog\n",stderr);TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));
    if(argc<4)return 2;
    std::wstring mode=argv[1],base=argv[2];DWORD value=wcstoul(argv[3],nullptr,10);
    if(mode==L"child")return child(base,value);
    if(argc<5)return 2;
    std::wstring control=argv[4];bool host=mode==L"host";if(!host)absent_host(base);
    Objects objects;if(!objects.create(base,value,!host))return 1;
    if(!host){local_contracts(objects,base);if(argc<6)return 2;
        std::wstring command=L"\""+std::wstring(argv[5])+L"\" child "+base+L" "+std::to_wstring(value);STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
        check(CreateProcessW(argv[5],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&process)!=FALSE,"start opposite-bitness IPC child");
        if(process.hProcess){DWORD code=999;check(WaitForSingleObject(process.hProcess,15000)==WAIT_OBJECT_0,"IPC child exits");GetExitCodeProcess(process.hProcess,&code);check(code==0,"opposite-bitness IPC child succeeds");CloseHandle(process.hThread);CloseHandle(process.hProcess);}
        check(*objects.page==value+1&&WaitForSingleObject(objects.event,0)==WAIT_OBJECT_0,"child updates memory and signals same-prefix parent");ResetEvent(objects.event);
        check(WaitForSingleObject(objects.sem,0)==WAIT_TIMEOUT,"child consumed only this prefix's semaphore");
    }
    marker(control+L".ready");while(!exists(control+L".stop"))Sleep(20);
    check(*objects.page==value+(host?0:1)&&WaitForSingleObject(objects.event,0)==WAIT_TIMEOUT,"host/sibling processes did not alter owned IPC state");
    if(host)check(WaitForSingleObject(objects.sem,0)==WAIT_OBJECT_0,"host semaphore untouched by both prefixes");
    printf("Named objects %zu-bit failures=%d\n",sizeof(void*)*8,failures);return failures?1:0;
}
