// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
using QueryKey=NTSTATUS(NTAPI*)(HANDLE,int,void*,ULONG,ULONG*);
using QueryObject=NTSTATUS(NTAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
static QueryKey query_key;
static QueryObject query_object;
static std::wstring key_name(HANDLE key){
    std::vector<BYTE> data(65536);ULONG needed=0;if(query_key(key,3,data.data(),(ULONG)data.size(),&needed)<0)return {};
    ULONG length=*(ULONG*)data.data();return std::wstring((wchar_t*)(data.data()+4),length/2);
}
static void inspect(HANDLE key,const char *label){
    ULONG needed=0,basic[16]{};NTSTATUS bs=query_object(key,0,basic,sizeof(basic),&needed);
    printf("HANDLE %s value=%p basic_status=%08lx granted=%08lx name=%ls\n",label,key,(ULONG)bs,basic[1],key_name(key).c_str());
    for(int cls:{5,7,8,9}){ULONG data[16]{};NTSTATUS status=query_key(key,cls,data,sizeof(data),&needed);printf("KEYINFO class=%d status=%08lx bytes=%lu data=%08lx,%08lx,%08lx,%08lx\n",cls,(ULONG)status,needed,data[0],data[1],data[2],data[3]);}
    HKEY child=nullptr;LSTATUS status=RegOpenKeyExW((HKEY)key,L"CLSID",0,KEY_READ,&child);
    printf("CHILD default status=%ld name=%ls\n",status,child?key_name(child).c_str():L"");if(child)RegCloseKey(child);
}
static int failures;
static bool host_mode;
static std::wstring token;
static void check(bool ok,const char *label){printf("%s %s error=%lu\n",ok?"PASS":"FAIL",label,GetLastError());if(!ok)++failures;}
static ULONG tags(HANDLE key){ULONG data=0,needed=0;return query_key(key,7,&data,sizeof(data),&needed)>=0?data:0;}
static bool correct(HANDLE parent,REGSAM view,REGSAM override_view=0){
    if(!view)view=sizeof(void*)==8?KEY_WOW64_64KEY:KEY_WOW64_32KEY;
    HKEY key=nullptr;std::wstring path=L"CLSID"+(host_mode?L"":L"\\"+token);
    LSTATUS status=RegOpenKeyExW((HKEY)parent,path.c_str(),0,KEY_READ|override_view,&key);bool result=false;
    if(!status){
        if(host_mode){auto name=key_name(key);std::transform(name.begin(),name.end(),name.begin(),towlower);result=(name.find(L"\\wow6432node\\clsid")!=name.npos)==(view==KEY_WOW64_32KEY);}
        else {DWORD value=0,type=0,size=sizeof(value);status=RegQueryValueExW(key,L"View",nullptr,&type,(BYTE*)&value,&size);result=!status&&type==REG_DWORD&&value==(view==KEY_WOW64_32KEY?32u:64u);}
        if(!result)printf("DETAIL relative read view=%lx override=%lx status=%ld path=%ls parent-tags=%lx\n",view,override_view,status,key_name(key).c_str(),tags(parent));RegCloseKey(key);
    }else printf("DETAIL relative open view=%lx override=%lx status=%ld parent=%p tags=%lx name=%ls\n",view,override_view,status,parent,tags(parent),key_name(parent).c_str());
    return result;
}
static HKEY parent_key(REGSAM view,unsigned kind=0){HKEY key=nullptr;RegOpenKeyExW(kind?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|view,&key);return key;}
struct Command {uint64_t handle;uint32_t operation,view,kind,options;};
struct Reply {uint64_t handle;uint32_t success,error,tags,reserved;};
static_assert(sizeof(Command)==24&&sizeof(Reply)==24);
static bool send_bytes(HANDLE pipe,const void *data,DWORD bytes){DWORD done=0;return WriteFile(pipe,data,bytes,&done,nullptr)&&done==bytes;}
static bool read_bytes(HANDLE pipe,void *data,DWORD bytes){DWORD done=0;return ReadFile(pipe,data,bytes,&done,nullptr)&&done==bytes;}
static int worker(HANDLE read,HANDLE write,DWORD parent_pid){
    std::vector<HANDLE> owned;Command command{};
    while(read_bytes(read,&command,sizeof(command))){
        Reply reply{};HANDLE handle=(HANDLE)(ULONG_PTR)command.handle;
        switch(command.operation){
        case 1:reply.success=correct(handle,command.view,command.options);reply.tags=tags(handle);break;
        case 2:{HKEY opened=parent_key(command.view,command.kind);reply.handle=(ULONG_PTR)opened;reply.success=opened!=nullptr;reply.tags=tags(opened);if(opened)owned.push_back(opened);break;}
        case 3:{HANDLE process=OpenProcess(PROCESS_DUP_HANDLE,FALSE,parent_pid),local=nullptr;
            BOOL ok=process&&DuplicateHandle(process,handle,GetCurrentProcess(),&local,command.kind,FALSE,command.options);reply.error=GetLastError();reply.success=ok&&correct(local,command.view);reply.tags=tags(local);if(local)CloseHandle(local);if(process)CloseHandle(process);break;}
        case 4:reply.success=CloseHandle(handle);reply.error=GetLastError();owned.erase(std::remove(owned.begin(),owned.end(),handle),owned.end());break;
        case 5:for(auto h:owned)CloseHandle(h);CloseHandle(read);CloseHandle(write);return 0;
        case 6:{HANDLE process=OpenProcess(PROCESS_DUP_HANDLE,FALSE,parent_pid),local=nullptr;
            BOOL ok=process&&DuplicateHandle(process,handle,GetCurrentProcess(),&local,ACCESS_SYSTEM_SECURITY,FALSE,command.options);reply.error=GetLastError();reply.success=!ok&&!local;if(local)CloseHandle(local);if(process)CloseHandle(process);break;}
        case 8:{HANDLE process=OpenProcess(PROCESS_DUP_HANDLE,FALSE,parent_pid),remote=nullptr;
            BOOL ok=process&&DuplicateHandle(GetCurrentProcess(),handle,process,&remote,0,FALSE,DUPLICATE_SAME_ACCESS);reply.error=GetLastError();reply.success=ok;reply.handle=(ULONG_PTR)remote;if(process)CloseHandle(process);break;}
        case 9:{if(host_mode)break;std::wstring path=L"CLSID\\"+token+L"\\TransferWrite"+std::to_wstring(command.kind);HKEY key=nullptr;
            LSTATUS status=RegCreateKeyExW((HKEY)handle,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&key,nullptr);DWORD value=command.view==KEY_WOW64_32KEY?32:64;
            if(!status)status=RegSetValueExW(key,L"View",0,REG_DWORD,(BYTE*)&value,sizeof(value));reply.success=!status;reply.error=status;if(key)RegCloseKey(key);break;}
        default:reply.error=ERROR_INVALID_PARAMETER;break;
        }
        if(!send_bytes(write,&reply,sizeof(reply)))break;
    }
    for(auto h:owned)CloseHandle(h);return 1;
}
struct Worker {
    PROCESS_INFORMATION process{};HANDLE write{},read{};
    ~Worker(){if(process.hProcess){Command quit{};quit.operation=5;send_bytes(write,&quit,sizeof(quit));if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,125);CloseHandle(process.hThread);CloseHandle(process.hProcess);}if(write)CloseHandle(write);if(read)CloseHandle(read);}
    bool start(const std::wstring &program){
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE child_read=nullptr,child_write=nullptr;
        if(!CreatePipe(&child_read,&write,&security,0))return false;
        if(!CreatePipe(&read,&child_write,&security,0)){CloseHandle(child_read);return false;}
        SetHandleInformation(write,HANDLE_FLAG_INHERIT,0);SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
        std::wstring command=L"\""+program+L"\" --worker "+std::to_wstring((ULONG_PTR)child_read)+L" "+std::to_wstring((ULONG_PTR)child_write)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+(host_mode?L"host":token);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        BOOL ok=CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process);CloseHandle(child_read);CloseHandle(child_write);return ok!=FALSE;
    }
    Reply call(unsigned op,HANDLE handle,REGSAM view,unsigned kind=0,unsigned options=0){
        Command command{(ULONG_PTR)handle,op,view,kind,options};Reply reply{};
        if(!send_bytes(write,&command,sizeof(command))||!read_bytes(read,&reply,sizeof(reply)))check(false,"worker command transport");
        return reply;
    }
};
static int transfers(const std::wstring &program){
    for(unsigned kind=0;kind<2;kind++)for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        printf("TRANSFER parent=%zu root=%u view=%lx child=%ls\n",sizeof(void*)*8,kind,view,program.c_str());
        HKEY original=parent_key(view,kind);check(original!=nullptr,"open source shared Classes handle");if(!original)continue;
        check(correct(original,view),"source relative default read selects requested view");
        check(SetHandleInformation(original,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT)!=FALSE,"mark Classes handle inheritable");
        Worker first,second;check(first.start(program)&&second.start(program),"launch only fixture-owned IPC workers");
        SetHandleInformation(original,HANDLE_FLAG_INHERIT,0);
        if(!first.process.hProcess||!second.process.hProcess){RegCloseKey(original);continue;}
        auto inherited=first.call(1,original,view);printf("INHERITED tags=%x\n",inherited.tags);check(inherited.success!=0,"inherited Classes handle preserves default view across process/bitness");
        if(!host_mode){
            unsigned id=GetCurrentProcessId()*8+kind*2+(view==KEY_WOW64_32KEY);
            check(first.call(9,original,view,id).success!=0,"create child key through inherited Classes handle");
            std::wstring path=L"CLSID\\"+token+L"\\TransferWrite"+std::to_wstring(id);HKEY value_key=nullptr;
            LSTATUS status=RegOpenKeyExW(original,path.c_str(),0,KEY_READ|view,&value_key);DWORD value=0,size=sizeof(value),type=0;
            if(!status)status=RegQueryValueExW(value_key,L"View",nullptr,&type,(BYTE*)&value,&size);
            check(!status&&type==REG_DWORD&&value==(view==KEY_WOW64_32KEY?32u:64u),"parent reads child write from requested private view");if(value_key)RegCloseKey(value_key);
            value_key=nullptr;status=RegOpenKeyExW(original,path.c_str(),0,KEY_READ|(view==KEY_WOW64_32KEY?KEY_WOW64_64KEY:KEY_WOW64_32KEY),&value_key);
            check(status==ERROR_FILE_NOT_FOUND,"child write does not appear in opposite registry view");if(value_key)RegCloseKey(value_key);
        }
        for(REGSAM explicit_view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY})check(first.call(1,original,explicit_view,0,explicit_view).success!=0,"explicit child view overrides inherited shared Classes handle");
        HANDLE remote=nullptr;
        check(DuplicateHandle(GetCurrentProcess(),original,first.process.hProcess,&remote,KEY_READ,FALSE,0)!=FALSE,"duplicate local source into remote target with read access");
        if(remote){auto reply=first.call(1,remote,view);printf("REMOTE tags=%x\n",reply.tags);check(reply.success!=0,"local-to-remote duplicate selects correct registry data");first.call(4,remote,view);}
        remote=nullptr;
        check(DuplicateHandle(GetCurrentProcess(),original,first.process.hProcess,&remote,KEY_ENUMERATE_SUB_KEYS,FALSE,0)!=FALSE,"duplicate view without KEY_QUERY_VALUE access");
        if(remote){check(first.call(1,remote,view).success!=0,"reduced-access transferred parent still selects correct child view");first.call(4,remote,view);}
        check(first.call(3,original,view,KEY_READ,0).success!=0,"remote-to-local duplicate initiated by child selects correct data");
        auto made=first.call(2,nullptr,view,kind);HANDLE child_key=(HANDLE)(ULONG_PTR)made.handle;check(made.success!=0,"worker opens requested shared Classes view");
        HANDLE local=nullptr;check(DuplicateHandle(first.process.hProcess,child_key,GetCurrentProcess(),&local,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"parent duplicates remote source locally");
        if(local){check(correct(local,view),"remote-to-local parent receives requested registry view");CloseHandle(local);}
        remote=nullptr;check(DuplicateHandle(first.process.hProcess,child_key,second.process.hProcess,&remote,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"broker duplicates remote source into another remote process");
        if(remote){check(second.call(1,remote,view).success!=0,"remote-to-remote duplicate selects correct registry data");second.call(4,remote,view);}
        check(first.call(4,child_key,view).success!=0,"worker closes original key");
        HKEY close_source=parent_key(view,kind);remote=nullptr;
        check(DuplicateHandle(GetCurrentProcess(),close_source,first.process.hProcess,&remote,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE)!=FALSE,"cross-process duplicate closes local source");
        if(remote){check(first.call(1,remote,view).success!=0,"close-source remote target retains view");first.call(4,remote,view);}
        ULONG count=0;check(query_key(close_source,7,&count,sizeof(count),&count)<0,"close-source handle is invalid in original process");
        close_source=parent_key(view,kind);check(first.call(3,close_source,view,0,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE).success!=0,"child-initiated remote close-source preserves target view");
        count=0;check(query_key(close_source,7,&count,sizeof(count),&count)<0,"child-closed source is invalid locally");
        // Remote duplication bypasses this process's bookkeeping. Fill the free
        // handle slots before inspecting children (whose temporary opens could
        // otherwise consume the target slot), then read actual opposite-view
        // values through the observed reused handle.
        REGSAM next=view==KEY_WOW64_32KEY?KEY_WOW64_64KEY:KEY_WOW64_32KEY;
        made=first.call(2,nullptr,next,kind);child_key=(HANDLE)(ULONG_PTR)made.handle;
        bool reused=false,correct_reuse=true;std::vector<HKEY> held;
        for(unsigned i=0;i<256;i++){auto reply=first.call(8,child_key,next);HKEY h=(HKEY)(ULONG_PTR)reply.handle;reused|=h==close_source;held.push_back(h);}
        for(auto h:held)correct_reuse=correct(h,next)&&correct_reuse;
        for(auto h:held)RegCloseKey(h);
        first.call(4,child_key,next);
        check(reused,"remote-close source handle value was actually reused");check(correct_reuse,"remote-close reuse never applies a stale registry view");
        HKEY failed=parent_key(view,kind);auto failure=first.call(6,failed,view);printf("DUPLICATE-FAILURE root=%u view=%lx close=0 error=%u\n",kind,view,failure.error);
        check(failure.success!=0,"failed remote duplication returns no target");check(correct(failed,view),"failed duplication without close preserves source data");RegCloseKey(failed);
        failed=parent_key(view,kind);failure=first.call(6,failed,view,0,DUPLICATE_CLOSE_SOURCE);printf("DUPLICATE-FAILURE root=%u view=%lx close=1 error=%u\n",kind,view,failure.error);
        check(failure.success!=0,"failed remote close-source duplication returns no target");
        count=0;check(query_key(failed,7,&count,sizeof(count),&count)<0,"failed remote close-source still closes source");
        HKEY protected_source=parent_key(view,kind);check(SetHandleInformation(protected_source,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)!=FALSE,"protect remote-close source");
        check(first.call(3,protected_source,view,0,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE).success!=0,"duplicate protected remote source");check(correct(protected_source,view),"protected remote source remains open with correct data");SetHandleInformation(protected_source,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);RegCloseKey(protected_source);
        HKEY implicit=nullptr,software=nullptr;LSTATUS implicit_status=RegOpenKeyExW(kind?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE,L"Software",0,KEY_READ|view,&software);
        if(!implicit_status)implicit_status=RegOpenKeyExW(software,L"Classes",0,KEY_READ,&implicit);if(software)RegCloseKey(software);
        printf("IMPLICIT root=%u requested-view=%lx parent-tags=%lx\n",kind,view,tags(implicit));
        // Native HKCU's Software->Classes link drops an implicit 64-bit tag.
        // With no explicit child flag it selects each receiving process's
        // default view; the native baseline exercises the same data/path rule.
        REGSAM implicit_view=kind&&view==KEY_WOW64_64KEY?0:view;
        check(!implicit_status&&correct(implicit,implicit_view),"implicit relative Classes open matches native view before transfer");
        remote=nullptr;if(implicit)check(DuplicateHandle(GetCurrentProcess(),implicit,first.process.hProcess,&remote,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"transfer implicitly selected Classes view");
        if(remote){check(first.call(1,remote,implicit_view).success!=0,"implicit parent transfer matches native inherited or default view");first.call(4,remote,view);}if(implicit)RegCloseKey(implicit);
        first.call(4,original,view);second.call(4,original,view);RegCloseKey(original);
    }
    printf("Registry transfer failures=%d\n",failures);return failures?1:0;
}
static DWORD WINAPI watchdog(void*){Sleep(90000);fputs("FAIL registry transfer watchdog\n",stderr);TerminateProcess(GetCurrentProcess(),124);return 0;}
static int seed_or_absent(bool seed){
    if(seed&&!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")){fputs("Refusing seed outside private runtime\n",stderr);return 2;}
    for(unsigned kind=0;kind<2;kind++)for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){
        std::wstring path=L"Software\\Classes\\CLSID\\"+token;HKEY key=nullptr;auto root=kind?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE;
        if(seed){LSTATUS status=RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE|view,nullptr,&key,nullptr);check(status==0,"create private view marker");if(key){DWORD value=view==KEY_WOW64_32KEY?32:64;check(RegSetValueExW(key,L"View",0,REG_DWORD,(BYTE*)&value,sizeof(value))==0,"write private view marker");RegCloseKey(key);}}
        else {LSTATUS status=RegOpenKeyExW(root,path.c_str(),0,KEY_READ|view,&key);check(status==ERROR_FILE_NOT_FOUND,"host or sibling has no fixture marker");if(key)RegCloseKey(key);}
    }return failures?1:0;
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));
    auto ntdll=GetModuleHandleW(L"ntdll.dll");query_key=(QueryKey)GetProcAddress(ntdll,"NtQueryKey");query_object=(QueryObject)GetProcAddress(ntdll,"NtQueryObject");
    if(argc>1&&!wcscmp(argv[1],L"--worker")){if(argc!=6)return 2;host_mode=!wcscmp(argv[5],L"host");token=argv[5];return worker((HANDLE)(ULONG_PTR)_wcstoui64(argv[2],nullptr,10),(HANDLE)(ULONG_PTR)_wcstoui64(argv[3],nullptr,10),wcstoul(argv[4],nullptr,10));}
    if(argc==4&&!wcscmp(argv[1],L"--transfers")){host_mode=!wcscmp(argv[3],L"host");token=argv[3];return transfers(argv[2]);}
    if(argc==3&&(!wcscmp(argv[1],L"--seed")||!wcscmp(argv[1],L"--absent"))){token=argv[2];return seed_or_absent(!wcscmp(argv[1],L"--seed"));}
    printf("ARCH %zu\n",sizeof(void*)*8);
    for(REGSAM view:{KEY_WOW64_32KEY,KEY_WOW64_64KEY}){HKEY key=nullptr;LSTATUS status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Classes",0,KEY_READ|view,&key);printf("OPEN view=%lx status=%ld\n",view,status);if(key){inspect(key,"classes");HANDLE duplicate=nullptr;DuplicateHandle(GetCurrentProcess(),key,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS);if(duplicate){inspect(duplicate,"duplicate");CloseHandle(duplicate);}RegCloseKey(key);}}
    return 0;
}
