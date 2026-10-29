// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone transport/lifecycle fixture using the real shared MediaSession.
#include "media-notifications.h"
#include <winioctl.h>
#include <cstdio>
using namespace wd;
static int failures;
static void test(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());fflush(stdout);if(!ok)++failures;}
static bool exists(const str &path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
static bool await_file(const str &path){ULONGLONG end=GetTickCount64()+10000;while(GetTickCount64()<end){if(exists(path))return true;Sleep(10);}return false;}
static std::string read(const str &path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return {};
    char buffer[4096];DWORD count=0;ReadFile(file,buffer,sizeof(buffer),&count,nullptr);CloseHandle(file);return std::string(buffer,count);
}
static bool await_text(const str &path,const char *expected){ULONGLONG end=GetTickCount64()+3000;while(GetTickCount64()<end){if(read(path)==expected)return true;Sleep(10);}return false;}
static HANDLE listener(HANDLE job,const str &exe,const str &folder,const str &name,const str &delay){
    str line=quote(exe)+L" --listener "+quote(folder+L"\\"+name+L".log")+L" "+quote(folder+L"\\"+name+L".ready")+L" "+quote(folder+L"\\"+name+L".stop")+L" "+delay;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi))return nullptr;
    if(!AssignProcessToJobObject(job,pi.hProcess)){TerminateProcess(pi.hProcess,1);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return nullptr;}
    ResumeThread(pi.hThread);CloseHandle(pi.hThread);return pi.hProcess;
}
static bool operation(MediaSession &session,DWORD code){BOOL result=FALSE;DWORD done=0;return session.ioctl(3,code,nullptr,0,nullptr,0,&done,nullptr,result)&&result;}
int wmain(int argc,wchar_t **argv){
    if(argc!=4)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    // Native listeners intentionally run without injected loader/desktop state.
    SetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr);
    try{
        Prefix prefix(argv[1]);MediaSession session(prefix,MediaSessionMode::create);str exe=argv[2],folder=argv[3];
        MediaState before{},after{};before.managed=after.managed=true;before.changes=0xfffffffeu;after.changes=2;
        test(media_notification_batch(before,after).transitions==4&&!media_notification_batch(before,after).gap,"counter wrap retains a bounded exact transition delta");
        before.changes=0;after.changes=4097;after.present=false;
        test(media_notification_batch(before,after).gap&&!media_notification_batch(before,after).transitions,"overrun produces an explicit gap rather than a synthetic event list");
        after.changes=2;test(media_notification_batch(before,after).gap,"inconsistent presence/count parity is never fabricated");
        HANDLE job=CreateJobObjectW(nullptr,nullptr);if(!job)return 3;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(job);return 3;}
        HANDLE slow=listener(job,exe,folder,L"slow",L"delay"),hung=nullptr;
        if(!slow||!await_file(folder+L"\\slow.ready")){if(slow)CloseHandle(slow);CloseHandle(job);return 4;}
        test(operation(session,IOCTL_STORAGE_EJECT_MEDIA)&&operation(session,IOCTL_STORAGE_LOAD_MEDIA),"real media state changes before observer attachment");
        {
            MediaNotifications notifications(session,job);Sleep(100);
            test(read(folder+L"\\slow.log").empty(),"observer baseline never replays earlier transitions to an existing window");
            test(operation(session,IOCTL_STORAGE_EJECT_MEDIA)&&await_file(folder+L"\\slow.ready.entered"),"real recipient enters its delayed window procedure");
            bool changed=true;for(unsigned n=0;n<5000;n++)changed=operation(session,n%2?IOCTL_STORAGE_EJECT_MEDIA:IOCTL_STORAGE_LOAD_MEDIA)&&changed;
            Sleep(800);
            test(changed&&session.snapshot(3).changes==5003&&read(folder+L"\\slow.log")=="EVENT 1 R VALID\n","observer discards an actual 5000-transition backlog with no invented deliveries");
            test(operation(session,IOCTL_STORAGE_LOAD_MEDIA)&&await_text(folder+L"\\slow.log","EVENT 1 R VALID\nEVENT 2 A VALID\n"),"observer resumes from current state after a history gap");
            hung=listener(job,exe,folder,L"hung",L"hang");
            test(hung&&await_file(folder+L"\\hung.ready"),"hung-recipient fixture is owned by the same private job");
            CloseHandle(job);job=nullptr;
            test(WaitForSingleObject(slow,0)==WAIT_TIMEOUT&&hung&&WaitForSingleObject(hung,0)==WAIT_TIMEOUT,"observer duplicate keeps the job valid after the caller closes its handle");
            test(operation(session,IOCTL_STORAGE_EJECT_MEDIA)&&await_file(folder+L"\\hung.ready.entered"),"shutdown starts while a recipient is still inside a five-second handler");
            ULONGLONG started=GetTickCount64();notifications.stop();ULONGLONG elapsed=GetTickCount64()-started;
            printf("Observer stop: %llu ms\n",elapsed);test(elapsed<750,"observer stop remains bounded by its short send timeout");
        }
        test(WaitForSingleObject(slow,2000)==WAIT_OBJECT_0&&hung&&WaitForSingleObject(hung,2000)==WAIT_OBJECT_0,"observer releases its duplicate job and owned test children on destruction");
        CloseHandle(slow);if(hung)CloseHandle(hung);if(job)CloseHandle(job);
        printf("Media notification service probe: %d failures\n",failures);return failures?1:0;
    }catch(const std::exception &error){fprintf(stderr,"Notification service fixture: %s\n",error.what());return 5;}
}
