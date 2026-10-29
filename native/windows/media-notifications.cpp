// SPDX-License-Identifier: LGPL-2.1-or-later
#include "media-notifications.h"
#include <dbt.h>
#include <process.h>
#include <algorithm>
#include <array>
#include <set>

namespace wd {
namespace {
constexpr DWORD poll_ms=20,send_ms=100,round_limit=32,process_limit=65536;
// This is the marker set by desktop.cpp for logical top-level game windows.
constexpr wchar_t virtual_root_property[]=L"Winedows.VirtualRoot";
struct Process {
    DWORD id;HANDLE handle;
    Process(DWORD pid,HANDLE object):id(pid),handle(object){}
    Process(Process&&other)noexcept:id(other.id),handle(other.handle){other.handle=nullptr;}
    ~Process(){if(handle)CloseHandle(handle);}
};
struct Recipient {HWND window;DWORD process;};
struct Selection {
    const std::vector<Process> &processes;std::vector<Recipient> windows;HANDLE stop;
    bool member(DWORD pid)const{return std::any_of(processes.begin(),processes.end(),[&](const Process &p){return p.id==pid;});}
    void add(HWND window){
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        if(member(pid)&&std::none_of(windows.begin(),windows.end(),[&](const Recipient &r){return r.window==window;}))windows.push_back({window,pid});
    }
    static BOOL CALLBACK top(HWND window,LPARAM context){
        auto self=reinterpret_cast<Selection*>(context);if(WaitForSingleObject(self->stop,0)==WAIT_OBJECT_0)return FALSE;
        self->add(window);return TRUE;
    }
    static BOOL CALLBACK child(HWND window,LPARAM context){
        auto self=reinterpret_cast<Selection*>(context);if(WaitForSingleObject(self->stop,0)==WAIT_OBJECT_0)return FALSE;
        if(GetPropW(window,virtual_root_property))self->add(window);
        return TRUE;
    }
};
// Static lifetime also protects a timed-out recipient that entered its window
// procedure before SendMessageTimeout returned. The actual targets are other
// processes; USER32 marshals this system message (including x86/x64 payloads).
const std::array<DEV_BROADCAST_VOLUME,26> payloads=[] {
    std::array<DEV_BROADCAST_VOLUME,26> result{};
    for(unsigned n=0;n<26;n++){
        auto &v=result[n];v.dbcv_size=sizeof(v);v.dbcv_devicetype=DBT_DEVTYP_VOLUME;
        v.dbcv_unitmask=1u<<n;v.dbcv_flags=DBTF_MEDIA;
    }
    return result;
}();
}
struct MediaNotifications::Impl {
    MediaSession &session;HANDLE job=nullptr,stop_event=nullptr,worker=nullptr;HWND desktop;
    std::array<MediaState,26> seen{};
    std::set<DWORD> warned_threads;
    bool query_warned=false;
    Impl(MediaSession &s,HWND window):session(s),desktop(window){}
    ~Impl(){
        stop();if(worker)CloseHandle(worker);if(stop_event)CloseHandle(stop_event);if(job)CloseHandle(job);
    }
    bool stopped()const{return WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0;}
    void stop(){if(stop_event)SetEvent(stop_event);if(worker)WaitForSingleObject(worker,INFINITE);}
    std::vector<Process> processes(){
        std::vector<Process> result;DWORD capacity=32;
        for(unsigned attempt=0;attempt<16;attempt++){
            std::vector<BYTE> bytes(offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST,ProcessIdList)+size_t(capacity)*sizeof(ULONG_PTR));
            auto list=reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(bytes.data());
            if(!QueryInformationJobObject(job,JobObjectBasicProcessIdList,list,DWORD(bytes.size()),nullptr)){
                if(GetLastError()==ERROR_MORE_DATA&&capacity<process_limit){capacity=std::min(process_limit,std::max(capacity*2,list->NumberOfAssignedProcesses));continue;}
                if(!query_warned){fprintf(stderr,"winedows: media notifications cannot enumerate their process job; no windows targeted\n");query_warned=true;}
                return result;
            }
            if(list->NumberOfProcessIdsInList>capacity)return result;
            for(DWORD n=0;n<list->NumberOfProcessIdsInList;n++){
                if(stopped())return result;
                DWORD pid=DWORD(list->ProcessIdList[n]);
                // Never notify this loader's own window: only its process job.
                if(!pid||pid==GetCurrentProcessId())continue;
                HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);BOOL belongs=FALSE;
                if(process&&IsProcessInJob(process,job,&belongs)&&belongs&&WaitForSingleObject(process,0)==WAIT_TIMEOUT)result.emplace_back(pid,process);
                else if(process)CloseHandle(process);
            }
            return result;
        }
        return result;
    }
    void send(const Selection &selection,int drive,bool present){
        // Same payload as Wine mountmgr.sys/device.c send_notify, with explicit
        // job recipients instead of BroadcastSystemMessage/HWND_BROADCAST.
        // https://learn.microsoft.com/windows/win32/devio/detecting-media-insertion-or-removal
        // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw
        for(const auto &recipient:selection.windows){
            if(stopped())return;
            DWORD pid=0,thread=GetWindowThreadProcessId(recipient.window,&pid);
            if(!thread||pid!=recipient.process)continue;
            auto process=std::find_if(selection.processes.begin(),selection.processes.end(),[&](const Process &p){return p.id==pid;});
            BOOL belongs=FALSE;
            if(process==selection.processes.end()||!IsProcessInJob(process->handle,job,&belongs)||!belongs||WaitForSingleObject(process->handle,0)!=WAIT_TIMEOUT)continue;
            DWORD_PTR answer=0;
            if(!SendMessageTimeoutW(recipient.window,WM_DEVICECHANGE,present?DBT_DEVICEARRIVAL:DBT_DEVICEREMOVECOMPLETE,
                                    reinterpret_cast<LPARAM>(&payloads[drive]),SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,send_ms,&answer)){
                if(warned_threads.size()>=256)warned_threads.clear();
                if(warned_threads.insert(thread).second)fprintf(stderr,"winedows: media notification recipient thread %lu did not complete within %lu ms; continuing\n",thread,send_ms);
            }
        }
    }
    void run(){
        try{
            while(!stopped()){
                std::array<MediaState,26> now{};bool changed=false;
                for(int drive=0;drive<26;drive++){
                    now[drive]=session.snapshot(drive);auto batch=media_notification_batch(seen[drive],now[drive]);
                    if(batch.gap){
                        fprintf(stderr,"winedows: media notification history gap on %c:; skipped %lu transitions and reset to current state\n",'A'+drive,now[drive].changes-seen[drive].changes);
                        seen[drive]=now[drive];
                    }else if(batch.transitions)changed=true;
                }
                if(changed){
                    auto members=processes();Selection selection{members,{},stop_event};EnumWindows(Selection::top,reinterpret_cast<LPARAM>(&selection));
                    if(desktop&&IsWindow(desktop))EnumChildWindows(desktop,Selection::child,reinterpret_cast<LPARAM>(&selection));
                    for(int drive=0;drive<26&&!stopped();drive++){
                        auto batch=media_notification_batch(seen[drive],now[drive]);
                        if(selection.windows.empty()){seen[drive]=now[drive];continue;}
                        for(DWORD n=0;n<std::min(batch.transitions,round_limit)&&!stopped();n++){
                            seen[drive].present=!seen[drive].present;++seen[drive].changes;
                            send(selection,drive,seen[drive].present);
                        }
                    }
                }
                WaitForSingleObject(stop_event,poll_ms);
            }
        }catch(const std::exception &error){fprintf(stderr,"winedows: media notifications stopped: %s\n",error.what());}
    }
    static unsigned __stdcall entry(void *self){static_cast<Impl*>(self)->run();return 0;}
};
MediaNotifications::MediaNotifications(MediaSession &session,HANDLE job,HWND desktop):impl(std::make_unique<Impl>(session,desktop)){
    if(!DuplicateHandle(GetCurrentProcess(),job,GetCurrentProcess(),&impl->job,0,FALSE,DUPLICATE_SAME_ACCESS))check(GetLastError(),"Retain notification process job");
    impl->stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!impl->stop_event)check(GetLastError(),"Create media notification stop event");
    for(int drive=0;drive<26;drive++)impl->seen[drive]=session.snapshot(drive);
    impl->worker=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,Impl::entry,impl.get(),0,nullptr));
    if(!impl->worker)throw std::runtime_error("Cannot start media notification observer");
}
MediaNotifications::~MediaNotifications()=default;
void MediaNotifications::stop(){impl->stop();}
}
