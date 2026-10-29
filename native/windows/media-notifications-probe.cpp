// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <dbt.h>
#include <process.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

static std::wstring log_path,ready_path,stop_path,delay_mode;
static HWND root_window,control_window,message_window;
static std::atomic<unsigned> events{0},bad_events{0},unexpected{0};
static std::atomic<bool> reenter{false},reentered{false},finished{false};
static int failures;
static bool delayed;
static void test(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());fflush(stdout);if(!ok)++failures;}
static std::wstring quote(const std::wstring &s){return L"\""+s+L"\"";}
static std::wstring env(const wchar_t *key){wchar_t text[32768]{};GetEnvironmentVariableW(key,text,32768);return text;}
static void write(const std::wstring &path,const std::string &text,bool append=false){
    HANDLE file=CreateFileW(path.c_str(),append?FILE_APPEND_DATA:GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,append?OPEN_ALWAYS:CREATE_ALWAYS,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD done=0;WriteFile(file,text.data(),DWORD(text.size()),&done,nullptr);CloseHandle(file);
}
static std::string read(const std::wstring &path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return {};
    char text[65536];DWORD got=0;ReadFile(file,text,sizeof(text),&got,nullptr);CloseHandle(file);return std::string(text,got);
}
static bool exists(const std::wstring &path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
static bool await_file(const std::wstring &path,DWORD timeout=10000){ULONGLONG end=GetTickCount64()+timeout;while(GetTickCount64()<end){if(exists(path))return true;Sleep(10);}return false;}
static HANDLE raw(){return CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);}
static bool operation(HANDLE handle,DWORD code){DWORD done=0;return DeviceIoControl(handle,code,nullptr,0,nullptr,0,&done,nullptr)!=FALSE;}
static LRESULT CALLBACK window_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_DEVICECHANGE&&(wp==DBT_DEVICEARRIVAL||wp==DBT_DEVICEREMOVECOMPLETE)){
        if(window!=root_window){++unexpected;write(log_path,"UNEXPECTED child/message-only recipient\n",true);return TRUE;}
        if(!delayed&&!delay_mode.empty()){
            delayed=true;write(ready_path+L".entered","1");
            Sleep(delay_mode==L"hang"?5000:600);
        }
        auto volume=reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp);
        bool valid=volume&&volume->dbcv_size==sizeof(*volume)&&volume->dbcv_devicetype==DBT_DEVTYP_VOLUME&&
                   !volume->dbcv_reserved&&volume->dbcv_unitmask==(1u<<3)&&volume->dbcv_flags==DBTF_MEDIA;
        if(!valid)++bad_events;
        unsigned number=events.fetch_add(1)+1;
        char line[160];snprintf(line,sizeof(line),"EVENT %u %s %s\n",number,wp==DBT_DEVICEARRIVAL?"A":"R",valid?"VALID":"BAD");write(log_path,line,true);
        // USER32 waits here while this application calls into its own media
        // backend. Notification dispatch must hold no media mutation lock.
        if(wp==DBT_DEVICEREMOVECOMPLETE&&reenter.exchange(false)){
            HANDLE device=raw();reentered=operation(device,IOCTL_STORAGE_LOAD_MEDIA);if(device!=INVALID_HANDLE_VALUE)CloseHandle(device);
        }
        return TRUE;
    }
    if(msg==WM_TIMER&&((!stop_path.empty()&&exists(stop_path))||finished.load())){DestroyWindow(root_window);return 0;}
    if(msg==WM_DESTROY&&window==root_window){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,msg,wp,lp);
}
static bool windows(){
    WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"WinedowsMediaNotificationProbe";
    if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    bool desktop=!env(L"WINEDOWS_DESKTOP_HWND").empty();
    root_window=CreateWindowExW(0,cls.lpszClassName,L"Media notification fixture",WS_POPUP|(desktop?WS_VISIBLE:0),0,0,1,1,nullptr,nullptr,cls.hInstance,nullptr);
    if(!root_window)return false;
    control_window=CreateWindowExW(0,cls.lpszClassName,L"control",WS_CHILD,0,0,1,1,root_window,nullptr,cls.hInstance,nullptr);
    message_window=CreateWindowExW(0,cls.lpszClassName,L"message",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    SetTimer(root_window,1,20,nullptr);return control_window&&message_window;
}
struct Child {HANDLE process=nullptr;std::wstring output;};
static Child spawn(const std::wstring &exe,const std::wstring &args){
    static unsigned seq;Child child;child.output=L"C:\\tests\\notify-command-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++seq)+L".log";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE output=CreateFileW(child.output.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,0,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=startup.hStdError=startup.hStdInput=output;
    PROCESS_INFORMATION pi{};std::wstring line=quote(exe)+L" "+args;
    if(output!=INVALID_HANDLE_VALUE&&CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi)){child.process=pi.hProcess;CloseHandle(pi.hThread);}
    if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
    return child;
}
static DWORD finish(Child &child,DWORD timeout=10000){
    DWORD code=~0u;if(!child.process)return code;
    if(WaitForSingleObject(child.process,timeout)==WAIT_OBJECT_0)GetExitCodeProcess(child.process,&code);else TerminateProcess(child.process,ERROR_TIMEOUT);
    CloseHandle(child.process);child.process=nullptr;return code;
}
static bool cli(const std::wstring &loader,const std::wstring &args,bool success=true){
    Child child=spawn(loader,L"drive "+args);DWORD code=finish(child);
    if(code!=(success?0u:1u))printf("CLI: %lu %s\n",code,read(child.output).c_str());
    return code==(success?0u:1u);
}
static unsigned lines(const std::wstring &path){std::string text=read(path);unsigned count=0;size_t at=0;while((at=text.find("EVENT ",at))!=std::string::npos){++count;at+=6;}return count;}
static bool sequence(const std::wstring &path,unsigned count){
    std::string expected;for(unsigned n=1;n<=count;n++)expected+="EVENT "+std::to_string(n)+(n%2?" R VALID\n":" A VALID\n");return read(path)==expected;
}
static bool await_count(unsigned count,const std::wstring &child_log,DWORD timeout=5000){
    ULONGLONG end=GetTickCount64()+timeout;
    while(GetTickCount64()<end){if(events.load()==count&&lines(child_log)==count&&sequence(log_path,count)&&sequence(child_log,count))return true;Sleep(10);}return false;
}
struct Exercise {std::wstring other,loader,b,bad;};
static unsigned __stdcall exercise(void *argument){
    auto &args=*static_cast<Exercise*>(argument);
    const std::wstring child_log=L"C:\\tests\\notification-child.log",child_ready=L"C:\\tests\\notification-child.ready",child_stop=L"C:\\tests\\notification-child.stop";
    Child child=spawn(args.other,L"--listener "+quote(child_log)+L" "+quote(child_ready)+L" "+quote(child_stop));
    test(child.process&&await_file(child_ready),"opposite-bitness listener is ready in the same job");
    Sleep(100);test(!events.load()&&!lines(child_log),"initial attachment does not fabricate arrival events");
    HANDLE device=raw();test(device!=INVALID_HANDLE_VALUE,"open only the virtual CD control device");
    test(operation(device,IOCTL_STORAGE_EJECT_MEDIA)&&await_count(1,child_log),"game eject sends exact removal payload to parent and child");
    test(operation(device,IOCTL_STORAGE_EJECT_MEDIA),"repeated game eject is accepted as a no-op");Sleep(150);
    test(events.load()==1&&lines(child_log)==1,"no-op eject sends no duplicate notification");
    test(operation(device,IOCTL_STORAGE_LOAD_MEDIA)&&await_count(2,child_log),"game load sends exact arrival payload to parent and child");
    PREVENT_MEDIA_REMOVAL lock{};lock.PreventMediaRemoval=TRUE;DWORD done=0;
    bool locked=DeviceIoControl(device,IOCTL_STORAGE_MEDIA_REMOVAL,&lock,sizeof(lock),nullptr,0,&done,nullptr)!=FALSE;
    bool rejected=!operation(device,IOCTL_STORAGE_EJECT_MEDIA)&&GetLastError()==ERROR_BUSY&&cli(args.loader,L"swap D "+quote(args.b),false);
    lock.PreventMediaRemoval=FALSE;bool unlocked=DeviceIoControl(device,IOCTL_STORAGE_MEDIA_REMOVAL,&lock,sizeof(lock),nullptr,0,&done,nullptr)!=FALSE;
    test(locked&&rejected&&unlocked&&cli(args.loader,L"swap D "+quote(args.bad),false),"locked eject/swap and malformed media fail before publication");Sleep(150);
    test(events.load()==2&&lines(child_log)==2,"locks and rejected changes remain silent");
    test(cli(args.loader,L"swap D "+quote(args.b))&&await_count(4,child_log),"live swap produces removal then arrival in both architectures");
    test(cli(args.loader,L"eject D")&&await_count(5,child_log)&&cli(args.loader,L"eject D"),"controller eject shares the same notification stream");Sleep(150);
    test(events.load()==5&&lines(child_log)==5,"controller no-op eject remains silent");
    test(cli(args.loader,L"load D")&&await_count(6,child_log)&&cli(args.loader,L"load D"),"controller load shares the same notification stream");Sleep(150);
    test(events.load()==6&&lines(child_log)==6,"controller no-op load remains silent");
    bool burst=true;for(unsigned n=0;n<10;n++)burst=operation(device,IOCTL_STORAGE_EJECT_MEDIA)&&operation(device,IOCTL_STORAGE_LOAD_MEDIA)&&burst;
    test(burst&&await_count(26,child_log),"rapid eject/load preserves all twenty ordered transitions");
    reenter=true;test(operation(device,IOCTL_STORAGE_EJECT_MEDIA)&&await_count(28,child_log)&&reentered.load(),"window callback may load media without a notification/media deadlock");
    std::wstring slow_log=L"C:\\tests\\notification-slow.log",slow_ready=L"C:\\tests\\notification-slow.ready",slow_stop=L"C:\\tests\\notification-slow.stop";
    Child slow=spawn(args.other,L"--listener "+quote(slow_log)+L" "+quote(slow_ready)+L" "+quote(slow_stop)+L" delay");
    bool slow_ok=slow.process&&await_file(slow_ready);ULONGLONG start=GetTickCount64();
    test(slow_ok&&operation(device,IOCTL_STORAGE_EJECT_MEDIA)&&await_count(29,child_log)&&await_file(slow_ready+L".entered"),"slow receiver cannot block game controls or healthy windows");
    while(lines(slow_log)<1&&GetTickCount64()-start<3000)Sleep(10);
    test(sequence(slow_log,1),"cross-process payload remains valid after sender timeout and delayed dereference");
    test(operation(device,IOCTL_STORAGE_LOAD_MEDIA)&&await_count(30,child_log),"healthy recipients continue after a timed-out window");
    ULONGLONG end=GetTickCount64()+2000;while(lines(slow_log)<2&&GetTickCount64()<end)Sleep(10);
    test(sequence(slow_log,2),"new child receives only transitions after its window exists");
    test(!bad_events.load()&&!unexpected.load(),"ordinary controls and message-only windows receive no media broadcast");
    if(!env(L"WINEDOWS_DESKTOP_HWND").empty())test(GetPropW(root_window,L"Winedows.VirtualRoot")!=nullptr,"contained virtual-desktop root receives media notifications");
    write(slow_stop,"1");write(child_stop,"1");test(finish(slow)==0&&finish(child)==0,"child listeners shut down cleanly after notifications");
    if(device!=INVALID_HANDLE_VALUE)CloseHandle(device);
    finished=true;return 0;
}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    bool listener=argc>=5&&std::wstring(argv[1])==L"--listener";
    if(listener){log_path=argv[2];ready_path=argv[3];stop_path=argv[4];if(argc==6)delay_mode=argv[5];}
    else if(argc==6&&std::wstring(argv[1])==L"--exercise")log_path=L"C:\\tests\\notification-main.log";
    else return 2;
    write(log_path,"");if(!windows())return 3;
    HANDLE worker=nullptr;Exercise args;
    if(listener)write(ready_path,"1");
    else{
        if(!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 4;
        args={argv[2],argv[3],argv[4],argv[5]};
        worker=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,exercise,&args,0,nullptr));if(!worker)return 5;
    }
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(worker){WaitForSingleObject(worker,INFINITE);CloseHandle(worker);}
    if(message_window)DestroyWindow(message_window);
    if(listener)return bad_events.load()||unexpected.load()?1:0;
    printf("Media notification probe: %d failures\n",failures);return failures?1:0;
}
