// SPDX-License-Identifier: LGPL-2.1-or-later
// Public Win32 observations for independently launched applications in one prefix.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>

using str=std::wstring;
static str token,role,key;
static void require(bool ok,const char *message){
    printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",message,GetLastError());fflush(stdout);
    if(!ok)throw std::runtime_error(message);
}
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    operator HANDLE()const{return value;}
};
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
struct View {volatile LONG *value=nullptr;~View(){if(value)UnmapViewOfFile((void*)value);}};
static str named(const wchar_t *kind){return L"Local\\CaskWin.SessionProbe."+token+L"."+kind;}
static str file(const str &suffix){return L"C:\\tests\\"+token+L"-"+suffix;}
static bool exists(const str &path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
static str env(const wchar_t *name){wchar_t value[32768];DWORD n=GetEnvironmentVariableW(name,value,32768);return n&&n<32768?str(value,n):str();}
static void marker(const str &suffix){
    str identity=env(L"WINEDOWS_MEDIA_SESSION");
    std::string text="pid="+std::to_string(GetCurrentProcessId())+"\nbits="+std::to_string(sizeof(void*)*8)+"\nmedia=";
    for(wchar_t c:identity){if(c<=0||c>=128)throw std::runtime_error("non-ASCII session identity");text.push_back((char)c);}text+='\n';
    Handle h(CreateFileW(file(suffix).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr));
    DWORD done=0;require(h.value!=INVALID_HANDLE_VALUE&&WriteFile(h,text.data(),(DWORD)text.size(),&done,nullptr)&&done==text.size(),"write phase marker");
}
template<class F>static void wait_until(F ready,const char *description){
    ULONGLONG deadline=GetTickCount64()+45000;
    while(!ready()){if(GetTickCount64()>=deadline)throw std::runtime_error(description);Sleep(20);}
    require(true,description);
}
static DWORD read_value(const wchar_t *name,bool missing_ok=false){
    Key h;LSTATUS s=RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&h.value);
    if(missing_ok&&s==ERROR_FILE_NOT_FOUND)return 0;
    require(s==ERROR_SUCCESS,"open private test key");DWORD value=0,size=sizeof(value),type=0;
    s=RegQueryValueExW(h.value,name,nullptr,&type,(BYTE*)&value,&size);
    if(missing_ok&&s==ERROR_FILE_NOT_FOUND)return 0;
    require(s==ERROR_SUCCESS&&type==REG_DWORD&&size==sizeof(value),"read live private registry value");return value;
}
static void write_value(const wchar_t *name,DWORD value){
    Key h;require(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&h.value,nullptr)==ERROR_SUCCESS,"create private test key");
    require(RegSetValueExW(h.value,name,0,REG_DWORD,(const BYTE*)&value,sizeof(value))==ERROR_SUCCESS,"write live private registry value");
}
static void absent(bool host){
    Key h;require(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_READ|KEY_WOW64_64KEY,&h.value)==ERROR_FILE_NOT_FOUND,"test registry key absent");
    SetLastError(0);Handle mutex(OpenMutexW(SYNCHRONIZE,FALSE,named(L"mutex").c_str()));
    require(!mutex.value&&GetLastError()==ERROR_FILE_NOT_FOUND,"test mutex absent");
    SetLastError(0);Handle event(OpenEventW(SYNCHRONIZE,FALSE,named(L"event").c_str()));
    require(!event.value&&GetLastError()==ERROR_FILE_NOT_FOUND,"test event absent");
    SetLastError(0);Handle mapping(OpenFileMappingW(FILE_MAP_READ,FALSE,named(L"mapping").c_str()));
    require(!mapping.value&&GetLastError()==ERROR_FILE_NOT_FOUND,"test section absent");
    if(!host)require(exists(L"R:\\state.txt"),"other prefix retains its initial virtual medium");
}
static Handle volume(){return Handle(CreateFileW(L"\\\\.\\R:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));}
static bool present(){auto h=volume();require(h.value!=INVALID_HANDLE_VALUE,"open configured virtual removable device");DWORD n=0,c=0;BOOL ok=DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&c,sizeof(c),&n,nullptr);if(!ok)require(GetLastError()==ERROR_NOT_READY,"absent virtual media status");return ok!=FALSE;}
static void eject(){auto h=volume();DWORD n=0;require(h.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(h,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0,&n,nullptr),"eject only the configured virtual medium");}
static void assert_no_text_import(){require(read_value(L"TextEdit",true)==0,"active text hive edit was not reimported");}
static int primary(int code){
    Handle mutex(CreateMutexW(nullptr,FALSE,named(L"mutex").c_str()));require(mutex.value&&GetLastError()!=ERROR_ALREADY_EXISTS,"primary creates new session mutex");
    Handle event(CreateEventW(nullptr,TRUE,FALSE,named(L"event").c_str()));require(event.value&&GetLastError()!=ERROR_ALREADY_EXISTS,"primary creates new session event");
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,named(L"mapping").c_str()));require(mapping.value&&GetLastError()!=ERROR_ALREADY_EXISTS,"primary creates new session section");
    View view;view.value=(LONG*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096);require(view.value!=nullptr,"primary maps shared section");
    InterlockedExchange(view.value,111);write_value(L"Primary",101);require(present(),"initial medium present");marker(L"primary.ready");
    require(WaitForSingleObject(event,45000)==WAIT_OBJECT_0,"independent joined process signaled event");
    require(read_value(L"Secondary")==202&&InterlockedCompareExchange(view.value,0,0)==222,"primary sees joined registry and section writes");
    require(!present()&&!exists(L"R:\\state.txt"),"primary sees joined media eject");assert_no_text_import();marker(L"primary.shared");
    wait_until([]{return exists(file(L"primary.stop"));},"primary release received");marker(L"primary.exiting");return code;
}
static int secondary(int code){
    require(read_value(L"Primary")==101,"joined launch sees primary live registry");assert_no_text_import();
    Handle mutex(CreateMutexW(nullptr,FALSE,named(L"mutex").c_str()));require(mutex.value&&GetLastError()==ERROR_ALREADY_EXISTS,"joined launch shares session mutex");
    Handle event(OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,named(L"event").c_str()));require(event.value!=nullptr,"joined launch opens session event");
    Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,named(L"mapping").c_str()));require(mapping.value!=nullptr,"joined launch opens session section");
    View view;view.value=(LONG*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096);require(view.value&&InterlockedCompareExchange(view.value,0,0)==111,"joined launch reads shared bytes");
    write_value(L"Secondary",202);InterlockedExchange(view.value,222);eject();require(SetEvent(event)!=FALSE,"joined launch signals primary");marker(L"secondary.ready");
    wait_until([]{return exists(file(L"secondary.stop"));},"secondary release received");
    require(read_value(L"Observer")==303,"secondary sees independent observer write after primary exit");assert_no_text_import();marker(L"secondary.exiting");return code;
}
static int observer(){
    require(read_value(L"Primary")==101&&read_value(L"Secondary")==202,"observer sees both live registry writes");assert_no_text_import();
    Handle mutex(OpenMutexW(SYNCHRONIZE,FALSE,named(L"mutex").c_str()));Handle event(OpenEventW(SYNCHRONIZE,FALSE,named(L"event").c_str()));
    Handle mapping(OpenFileMappingW(FILE_MAP_READ,FALSE,named(L"mapping").c_str()));
    View view;view.value=(LONG*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,4096);
    require(mutex.value&&event.value&&WaitForSingleObject(event,0)==WAIT_OBJECT_0&&view.value&&*view.value==222,"objects survive first application exit");
    require(!present()&&!exists(L"R:\\state.txt"),"media state survives first application exit");write_value(L"Observer",303);marker(L"observer.ready");return 19;
}
static int persisted(){
    require(read_value(L"Primary")==101&&read_value(L"Secondary")==202&&read_value(L"Observer")==303,"final registry writes survive new session");assert_no_text_import();
    Handle mutex(OpenMutexW(SYNCHRONIZE,FALSE,named(L"mutex").c_str()));require(!mutex.value,"finished session object namespace retired");
    require(present()&&exists(L"R:\\state.txt"),"new session starts configured medium present");marker(L"persisted.ready");return 0;
}
static int race(int code){
    Handle mutex(CreateMutexW(nullptr,FALSE,named(L"mutex").c_str()));require(mutex.value!=nullptr,"race opens shared mutex");
    require(WaitForSingleObject(mutex,20000)==WAIT_OBJECT_0,"race acquires shared mutex");
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,named(L"mapping").c_str()));require(mapping.value!=nullptr,"race opens shared section");
    View view;view.value=(LONG*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096);require(view.value!=nullptr,"race maps section");
    DWORD count=read_value(L"RaceCount",true);require(count<2&&*view.value==(LONG)count,"race sees coherent prior admission");
    write_value(L"RaceCount",count+1);InterlockedExchange(view.value,(LONG)count+1);require(ReleaseMutex(mutex)!=FALSE,"race releases mutex");marker(role+L".ready");
    wait_until([]{return exists(file(L"race.stop"));},"race release received");
    require(read_value(L"RaceCount")==2&&InterlockedCompareExchange(view.value,0,0)==2,"both racing independent launches shared state");return code;
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    try{
        require(argc==4,"expected ROLE TOKEN EXITCODE");role=argv[1];token=argv[2];
        require(token.size()==32&&token.find_first_not_of(L"0123456789abcdef")==str::npos,"unique safe fixture token");key=L"Software\\CaskWin\\SessionProbe\\"+token;
        int code=_wtoi(argv[3]);require(code>=0&&code<100,"bounded exit code");
        if(role==L"host-absent"){absent(true);return 0;}
#ifdef _WIN64
        const wchar_t *shim=L"winedows64.dll";
#else
        const wchar_t *shim=L"winedows32.dll";
#endif
        // Mutating roles must never run against host HKCU or a host drive.
        require(GetModuleHandleW(shim)!=nullptr&&!env(L"WINEDOWS_PREFIX").empty()&&!env(L"WINEDOWS_MEDIA_SESSION").empty(),"private injected runtime present before any mutation");
        if(role==L"absent"){absent(false);marker(L"absent.ready");return 0;}
        if(role==L"primary")return primary(code);
        if(role==L"secondary")return secondary(code);
        if(role==L"observer")return observer();
        if(role==L"persisted")return persisted();
        if(role==L"race-a"||role==L"race-b")return race(code);
        throw std::runtime_error("unknown fixture role");
    }catch(const std::exception &e){fprintf(stderr,"session-probe: %s\n",e.what());return 100;}
}
