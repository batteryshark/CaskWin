// SPDX-License-Identifier: LGPL-2.1-or-later
#include "prefix-session.h"
#include "media-state.h"
#include <array>

namespace wd {
namespace {
constexpr DWORD magic=0x4357534e,version=1;
constexpr wchar_t endpoint_file[]=L"\\.winedows\\session.control";
constexpr const wchar_t *runtime_files[]={L"wine.exe",L"wine32.exe",L"wine64.exe",L"winedows32.dll",L"winedows64.dll"};
struct Handle {
    HANDLE h=nullptr;
    Handle()=default;
    explicit Handle(HANDLE value):h(value){}
    ~Handle(){reset();}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    void reset(HANDLE value=nullptr){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=value;}
};
struct FileIdentity {DWORD present,volume,index_high,index_low,size_high,size_low,time_high,time_low;};
struct alignas(8) Endpoint {
    DWORD magic,version,bytes,pid;
    FILETIME created;
    ULONGLONG desktop;
    FileIdentity runtime[5];
    wchar_t media[96],root[32768],directory[32768],desktop_spec[512];
};
static_assert(sizeof(FileIdentity)==32&&sizeof(Endpoint)==132480,"Prefix session wire ABI changed");
template<size_t N> void put(wchar_t (&out)[N],const str &value){
    if(value.size()>=N)throw std::runtime_error("Prefix session field is too long");
    memcpy(out,value.c_str(),(value.size()+1)*sizeof(wchar_t));
}
template<size_t N> str field(const wchar_t (&value)[N]){
    size_t length=wcsnlen_s(value,N);if(length==N)throw std::runtime_error("Invalid prefix session string");return str(value,length);
}
str job_name(const str &media){
    const str head=L"Local\\Winedows.Media.";
    if(media.size()!=head.size()+32||media.compare(0,head.size(),head))throw std::runtime_error("Invalid prefix session identity");
    for(size_t i=head.size();i<media.size();i++)if(!((media[i]>=L'0'&&media[i]<=L'9')||(media[i]>=L'a'&&media[i]<=L'f')))
        throw std::runtime_error("Invalid prefix session token");
    return L"Local\\CaskWin.Session."+media.substr(head.size());
}
void ordinary_storage(const str &root){
    mkdirs(root+L"\\.winedows");DWORD attrs=GetFileAttributesW((root+L"\\.winedows").c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_DIRECTORY)||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Prefix session requires ordinary private storage");
}
HANDLE lock_file(const str &path,bool wait){
    ULONGLONG deadline=GetTickCount64()+30000;
    for(;;){
        HANDLE h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(h!=INVALID_HANDLE_VALUE){
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))){CloseHandle(h);throw std::runtime_error("Invalid prefix session lock storage");}
            return h;
        }
        DWORD error=GetLastError();if(error!=ERROR_SHARING_VIOLATION)check(error,"Lock prefix session");
        if(!wait)return nullptr;
        if(GetTickCount64()>=deadline)throw std::runtime_error("Prefix startup is still busy; retry this launch");
        Sleep(25);
    }
}
FILETIME creation_time(HANDLE process){
    FILETIME created{},exited{},kernel{},user{};if(!GetProcessTimes(process,&created,&exited,&kernel,&user))check(GetLastError(),"Read prefix owner lifetime");return created;
}
std::vector<BYTE> token_data(HANDLE token,TOKEN_INFORMATION_CLASS kind){
    DWORD size=0;GetTokenInformation(token,kind,nullptr,0,&size);if(!size)check(GetLastError(),"Size prefix owner token");
    std::vector<BYTE> data(size);if(!GetTokenInformation(token,kind,data.data(),size,&size))check(GetLastError(),"Read prefix owner token");return data;
}
void same_logon(HANDLE process){
    Handle owner,self;
    if(!OpenProcessToken(process,TOKEN_QUERY,&owner.h)||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&self.h))check(GetLastError(),"Open prefix owner token");
    auto a=token_data(owner.h,TokenUser),b=token_data(self.h,TokenUser);
    if(!EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid))throw std::runtime_error("Active prefix belongs to another user");
    a=token_data(owner.h,TokenStatistics);b=token_data(self.h,TokenStatistics);
    if(memcmp(&reinterpret_cast<TOKEN_STATISTICS*>(a.data())->AuthenticationId,&reinterpret_cast<TOKEN_STATISTICS*>(b.data())->AuthenticationId,sizeof(LUID)))
        throw std::runtime_error("Active prefix belongs to another logon");
    a=token_data(owner.h,TokenSessionId);b=token_data(self.h,TokenSessionId);
    if(*reinterpret_cast<DWORD*>(a.data())!=*reinterpret_cast<DWORD*>(b.data()))throw std::runtime_error("Active prefix belongs to another Windows session");
    a=token_data(owner.h,TokenIntegrityLevel);b=token_data(self.h,TokenIntegrityLevel);
    if(!EqualSid(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(a.data())->Label.Sid,reinterpret_cast<TOKEN_MANDATORY_LABEL*>(b.data())->Label.Sid))
        throw std::runtime_error("Active prefix has a different integrity level");
}
void configure_job(HANDLE job){
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))check(GetLastError(),"Configure prefix process job");
}
DWORD active_processes(HANDLE job){
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr))check(GetLastError(),"Query prefix process job");return info.ActiveProcesses;
}
}
struct PrefixSession::Impl {
    str root,runtime;
    bool owner=false,published=false;
    Handle admission,lock,shared_job,launch_job,owner_process;
    std::array<Handle,5> runtime_locks;
    STARTUPINFOEXW startup{};
    std::vector<BYTE> attributes;
    HANDLE job_list[2]{};
    bool attributes_ready=false;
    std::unique_ptr<Endpoint> endpoint=std::make_unique<Endpoint>();
    explicit Impl(const Prefix &p):root(p.root),runtime(module_dir()){}
    ~Impl(){
        if(attributes_ready)DeleteProcThreadAttributeList(startup.lpAttributeList);
        // Admission outlives endpoint removal, job closure and session.lock.
        if(published&&!admission.h){
            try{admission.reset(lock_file(root+L"\\.winedows\\session.start.lock",true));}
            catch(...){if(shared_job.h)TerminateJobObject(shared_job.h,ERROR_PROCESS_ABORTED);}
        }
        if(published)DeleteFileW((root+endpoint_file).c_str());
        launch_job.reset();shared_job.reset();lock.reset();
    }
    void runtime_identity(FileIdentity (&out)[5]){
        for(size_t i=0;i<runtime_locks.size();i++){
            runtime_locks[i].reset(CreateFileW((runtime+L"\\"+runtime_files[i]).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            if(runtime_locks[i].h==INVALID_HANDLE_VALUE){DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND)continue;check(e,"Lock active CaskWin runtime");}
            BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(runtime_locks[i].h,&info))check(GetLastError(),"Identify active CaskWin runtime");
            out[i]={1,info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,info.nFileSizeHigh,info.nFileSizeLow,info.ftLastWriteTime.dwHighDateTime,info.ftLastWriteTime.dwLowDateTime};
        }
    }
    void join(){
        Handle file(CreateFileW((root+endpoint_file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(file.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Prefix is busy but has no compatible active CaskWin session");
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};DWORD got=0;auto &e=*endpoint;
        if(!GetFileInformationByHandle(file.h,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!GetFileSizeEx(file.h,&size)||size.QuadPart!=sizeof(e)||
           !ReadFile(file.h,&e,sizeof(e),&got,nullptr)||got!=sizeof(e)||e.magic!=magic||e.version!=version||e.bytes!=sizeof(e))
            throw std::runtime_error("Invalid or incompatible active prefix session");
        if(_wcsicmp(field(e.root).c_str(),root.c_str())||_wcsicmp(field(e.directory).c_str(),runtime.c_str()))
            throw std::runtime_error("Active prefix requires the same prefix path and CaskWin runtime directory");
        str media=field(e.media);job_name(media);field(e.desktop_spec);
        owner_process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,e.pid));
        if(!owner_process.h)check(GetLastError(),"Open active prefix owner");
        auto created=creation_time(owner_process.h);
        if(WaitForSingleObject(owner_process.h,0)!=WAIT_TIMEOUT||memcmp(&created,&e.created,sizeof(created)))throw std::runtime_error("Stale prefix session owner");
        same_logon(owner_process.h);
        std::vector<wchar_t> image(32768);DWORD chars=DWORD(image.size());
        if(!QueryFullProcessImageNameW(owner_process.h,0,image.data(),&chars))check(GetLastError(),"Identify prefix owner runtime");
        str path(image.data(),chars),base=path.substr(path.find_last_of(L"\\/")+1);
        if(_wcsicmp(directory(path).c_str(),runtime.c_str())||(_wcsicmp(base.c_str(),L"wine.exe")&&_wcsicmp(base.c_str(),L"wine32.exe")&&_wcsicmp(base.c_str(),L"wine64.exe")))
            throw std::runtime_error("Prefix owner is not this CaskWin runtime");
        FileIdentity files[5]{};runtime_identity(files);
        if(memcmp(files,e.runtime,sizeof(files)))throw std::runtime_error("CaskWin runtime changed during the active prefix session");
        shared_job.reset(OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS|JOB_OBJECT_QUERY,FALSE,job_name(media).c_str()));
        if(!shared_job.h)check(GetLastError(),"Open active prefix process job");
        launch_job.reset(CreateJobObjectW(nullptr,nullptr));if(!launch_job.h)check(GetLastError(),"Create joining launch job");configure_job(launch_job.h);
        if(!SetEnvironmentVariableW(media_session_environment_name,media.c_str()))check(GetLastError(),"Attach prefix media identity");
    }
};
PrefixSession::PrefixSession(const Prefix &p,bool allow_join):impl(std::make_unique<Impl>(p)){
    ordinary_storage(p.root);
    impl->admission.reset(lock_file(p.root+L"\\.winedows\\session.start.lock",true));
    impl->lock.reset(lock_file(p.root+L"\\.winedows\\session.lock",false));
    impl->owner=impl->lock.h!=nullptr;
    if(!impl->owner){if(!allow_join)throw std::runtime_error("Prefix is busy; configuration and registry maintenance require an idle prefix");impl->join();}
}
PrefixSession::~PrefixSession()=default;
bool PrefixSession::owner() const{return impl->owner;}
void PrefixSession::create_job(const str &media){
    if(!owner())return;
    FileIdentity files[5]{};impl->runtime_identity(files);memcpy(impl->endpoint->runtime,files,sizeof(files));
    impl->shared_job.reset(CreateJobObjectW(nullptr,job_name(media).c_str()));
    if(!impl->shared_job.h)check(GetLastError(),"Create prefix process job");
    if(GetLastError()==ERROR_ALREADY_EXISTS){impl->shared_job.reset();throw std::runtime_error("Prefix process job identity already exists");}
    configure_job(impl->shared_job.h);put(impl->endpoint->media,media);
}
HANDLE PrefixSession::job() const{return impl->shared_job.h;}
void PrefixSession::publish(const str &desktop,HWND window){
    if(!owner())throw std::runtime_error("Only the prefix owner can publish its session");
    auto &e=*impl->endpoint;e.magic=magic;e.version=version;e.bytes=sizeof(e);e.pid=GetCurrentProcessId();e.created=creation_time(GetCurrentProcess());e.desktop=(ULONG_PTR)window;
    put(e.root,impl->root);put(e.directory,impl->runtime);put(e.desktop_spec,desktop);
    str path=impl->root+endpoint_file;
    DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))throw std::runtime_error("Prefix session endpoint is redirected");
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.h==INVALID_HANDLE_VALUE)check(GetLastError(),"Publish prefix session");
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.h,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Prefix session endpoint is redirected");
    DWORD written=0;if(!WriteFile(file.h,&e,sizeof(e),&written,nullptr)||written!=sizeof(e)||!FlushFileBuffers(file.h))check(GetLastError()?GetLastError():ERROR_WRITE_FAULT,"Write prefix session");
    impl->published=true;
}
void PrefixSession::attach_desktop(const str &explicit_specification){
    auto &e=*impl->endpoint;
    if(!explicit_specification.empty()&&_wcsicmp(explicit_specification.c_str(),field(e.desktop_spec).c_str()))
        throw std::runtime_error("Active prefix uses a different desktop; omit --desktop to join it or wait for the prefix to exit");
    HWND window=(HWND)(ULONG_PTR)e.desktop;
    if(e.desktop){
        DWORD pid=0;wchar_t cls[64]{};GetWindowThreadProcessId(window,&pid);
        if(e.desktop>MAXDWORD||pid!=e.pid||!GetClassNameW(window,cls,64)||wcscmp(cls,L"WinedowsVirtualDesktop"))throw std::runtime_error("Active prefix desktop is no longer available");
    }
    if(!SetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",window?std::to_wstring((ULONG_PTR)window).c_str():nullptr))check(GetLastError(),"Attach active prefix desktop");
}
STARTUPINFOEXW &PrefixSession::startup(){
    if(impl->attributes_ready)throw std::runtime_error("Prefix process startup was already prepared");
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    if(!size)check(GetLastError(),"Size prefix startup attributes");
    impl->attributes.resize(size);auto &info=impl->startup;info.StartupInfo.cb=sizeof(info);
    info.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(impl->attributes.data());
    if(!InitializeProcThreadAttributeList(info.lpAttributeList,1,0,&size))check(GetLastError(),"Initialize prefix startup attributes");
    impl->attributes_ready=true;impl->job_list[0]=impl->shared_job.h;impl->job_list[1]=impl->launch_job.h;
    // Windows 10 assigns the ordered jobs as part of CreateProcess. There is
    // no unowned suspended-child interval if the loader is terminated during
    // Detours injection or before ResumeThread.
    if(!UpdateProcThreadAttribute(info.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,impl->job_list,(owner()?1:2)*sizeof(HANDLE),nullptr,nullptr))
        check(GetLastError(),"Configure atomic prefix job admission");
    return info;
}
void PrefixSession::started(){
    if(!owner()&&WaitForSingleObject(impl->owner_process.h,0)!=WAIT_TIMEOUT)throw std::runtime_error("Prefix owner exited during launch");
    if(!owner())impl->shared_job.reset(); // Owner death must still close the session job.
}
void PrefixSession::release_admission(){impl->admission.reset();}
void PrefixSession::wait(){
    if(!owner()){while(active_processes(impl->launch_job.h))Sleep(50);return;}
    for(;;){
        if(!active_processes(impl->shared_job.h)){
            impl->admission.reset(lock_file(impl->root+L"\\.winedows\\session.start.lock",false));
            if(impl->admission.h){if(!active_processes(impl->shared_job.h))return;impl->admission.reset();}
        }
        Sleep(50);
    }
}
}
