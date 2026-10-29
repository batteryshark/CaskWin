// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void test(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static std::wstring quote(const std::wstring &s){return L"\""+s+L"\"";}
static std::wstring environment(const wchar_t *key){wchar_t value[32768]{};GetEnvironmentVariableW(key,value,32768);return value;}
static HANDLE raw(){return CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);}
static HANDLE file(){return CreateFileW(L"D:\\README.TXT",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);}
static HANDLE directory(){return CreateFileW(L"D:\\",FILE_LIST_DIRECTORY|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);}
static bool read(HANDLE h,const char *expected){LARGE_INTEGER zero{};char bytes[22]{};DWORD got=0;return SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)&&ReadFile(h,bytes,22,&got,nullptr)&&got==22&&!memcmp(bytes,expected,22);}
static bool stale(HANDLE h,DWORD error=ERROR_MEDIA_CHANGED){char byte=0x55;DWORD got=~0u;BOOL result=ReadFile(h,&byte,1,&got,nullptr);return !result&&GetLastError()==error&&!got&&byte==0x55;}
static DWORD frame(const BYTE *p){return p[1]*4500+p[2]*75+p[3];}
static bool counter(HANDLE h,DWORD wanted){DWORD count=~0u,got=0;return DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&count,4,&got,nullptr)&&got==4&&count==wanted;}
static DWORD count_now(HANDLE h){DWORD value=~0u,got=0;if(!DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&value,4,&got,nullptr)||got!=4)return ~0u;return value;}
static bool lock(HANDLE h,bool value){PREVENT_MEDIA_REMOVAL request{};request.PreventMediaRemoval=value;DWORD got=0;return DeviceIoControl(h,IOCTL_STORAGE_MEDIA_REMOVAL,&request,sizeof(request),nullptr,0,&got,nullptr)!=FALSE;}
static std::wstring final_path(HANDLE h){wchar_t text[32768]{};DWORD n=GetFinalPathNameByHandleW(h,text,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);return n&&n<32768?std::wstring(text,n):std::wstring();}
static bool current(wchar_t disc){
    const char *bytes=disc==L'A'?"Hello from Winedows!\r\n":disc==L'B'?"Different disc B data\n":"Third disc C contents\n";
    const wchar_t *label=disc==L'A'?L"SWAP_DISC_A":disc==L'B'?L"SWAP_DISC_B":L"SWAP_DISC_C";
    wchar_t got_label[64]{};bool ok=GetVolumeInformationW(L"D:\\",got_label,64,nullptr,nullptr,nullptr,nullptr,0)&&!wcscmp(label,got_label);
    HANDLE data=file(),device=raw();ok=ok&&data!=INVALID_HANDLE_VALUE&&device!=INVALID_HANDLE_VALUE&&read(data,bytes);
    DWORD sectors=0,sector_bytes=0,free_clusters=~0u,total_clusters=0;
    ok=ok&&GetDiskFreeSpaceW(L"D:\\",&sectors,&sector_bytes,&free_clusters,&total_clusters)&&sectors==1&&sector_bytes==2048&&!free_clusters&&total_clusters==48;
    if(data!=INVALID_HANDLE_VALUE){
        std::wstring physical=final_path(data);if(physical.rfind(L"\\\\?\\",0)==0)physical.erase(0,4);auto slash=physical.find_last_of(L'\\');
        if(slash==std::wstring::npos)ok=false;
        else {physical.resize(slash);ok=ok&&GetDiskFreeSpaceW(physical.c_str(),&sectors,&sector_bytes,&free_clusters,&total_clusters)&&sectors==1&&sector_bytes==2048&&!free_clusters&&total_clusters==48;}
    }
    if(device!=INVALID_HANDLE_VALUE){
        LARGE_INTEGER size{};ok=ok&&GetFileSizeEx(device,&size)&&size.QuadPart==(disc==L'A'?64:disc==L'B'?72:48)*2048;
        CDROM_TOC toc{};DWORD done=0;BOOL toc_ok=DeviceIoControl(device,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&done,nullptr);
        if(disc==L'C')ok=ok&&!toc_ok; // An ISO-only replacement must not retain B's CDDA manifest.
        else {
            ok=ok&&toc_ok&&toc.LastTrack==3&&frame(toc.TrackData[1].Address)==DWORD(disc==L'A'?219:224)&&frame(toc.TrackData[3].Address)==DWORD(disc==L'A'?256:273);
            RAW_READ_INFO request{};request.DiskOffset.QuadPart=(disc==L'A'?69:74)*2048;request.SectorCount=1;request.TrackMode=CDDA;
            BYTE pcm[2352]{};ok=ok&&DeviceIoControl(device,IOCTL_CDROM_RAW_READ,&request,sizeof(request),pcm,sizeof(pcm),&done,nullptr)&&done==sizeof(pcm);
            for(unsigned n=0;n<588&&ok;n++){
                short sample=short(disc==L'A'?66*64+n:-(74*80+int(n))),opposite=short(-sample);
                ok=pcm[n*4]==BYTE(sample)&&pcm[n*4+1]==BYTE(static_cast<unsigned short>(sample)>>8)&&pcm[n*4+2]==BYTE(opposite)&&pcm[n*4+3]==BYTE(static_cast<unsigned short>(opposite)>>8);
            }
        }
    }
    if(data!=INVALID_HANDLE_VALUE)CloseHandle(data);
    if(device!=INVALID_HANDLE_VALUE)CloseHandle(device);
    return ok;
}
struct Child {HANDLE process=nullptr;std::wstring log;};
static Child spawn(const wchar_t *exe,const std::wstring &args,bool inherit=true){
    static unsigned sequence;Child child;child.log=L"C:\\tests\\swap-child-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence)+L".log";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE output=CreateFileW(child.log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,0,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=startup.hStdError=startup.hStdInput=output;
    PROCESS_INFORMATION pi{};std::wstring line=quote(exe)+L" "+args;
    if(output!=INVALID_HANDLE_VALUE&&CreateProcessW(exe,line.data(),nullptr,nullptr,inherit,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&pi)){
        child.process=pi.hProcess;CloseHandle(pi.hThread);
    }
    if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
    return child;
}
static DWORD finish(Child &child){
    DWORD code=~0u;if(!child.process)return code;
    if(WaitForSingleObject(child.process,30000)==WAIT_OBJECT_0)GetExitCodeProcess(child.process,&code);else TerminateProcess(child.process,ERROR_TIMEOUT);
    CloseHandle(child.process);child.process=nullptr;return code;
}
static std::string log_text(const Child &child){
    HANDLE h=CreateFileW(child.log.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)return {};
    char data[16384];DWORD n=0;ReadFile(h,data,sizeof(data),&n,nullptr);CloseHandle(h);return std::string(data,n);
}
static bool cli(const wchar_t *loader,const std::wstring &args,bool success=true,const char *message=nullptr){
    Child child=spawn(loader,L"drive "+args);DWORD code=finish(child);std::string output=log_text(child);
    bool ok=success?code==0:code==1;if(message)ok=ok&&output.find(message)!=std::string::npos;
    if(!ok)printf("CLI failure: exit=%lu output=%s\n",code,output.c_str());
    return ok;
}
struct Request {DWORD magic,version,operation,drive,root_chars,source_chars,label_chars;};
struct Response {DWORD magic,version,error,changes,message_chars;};
struct Reply {DWORD error=~0u,changes=~0u;std::wstring message;};
static Reply rpc(DWORD operation,const std::wstring &source=L"",bool wrong_prefix=false,bool bad_length=false){
    std::wstring root=environment(L"WINEDOWS_PREFIX"),session=environment(L"WINEDOWS_MEDIA_SESSION");
    if(wrong_prefix)root+=L"-other-prefix";
    std::wstring name=L"\\\\.\\pipe\\LOCAL\\"+session.substr(6)+L".Control";
    if(!WaitNamedPipeW(name.c_str(),5000))return {};
    HANDLE pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(pipe==INVALID_HANDLE_VALUE)return {};
    DWORD mode=PIPE_READMODE_MESSAGE;SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr);
    Request header{0x574d4350,1,operation,3,DWORD(root.size()),DWORD(source.size()),0};if(bad_length)header.source_chars=0xffffffff;
    std::vector<BYTE> packet(sizeof(header)+2*(root.size()+source.size()));memcpy(packet.data(),&header,sizeof(header));memcpy(packet.data()+sizeof(header),root.data(),root.size()*2);memcpy(packet.data()+sizeof(header)+root.size()*2,source.data(),source.size()*2);
    DWORD done=0;Reply result;BYTE response[8192];
    if(WriteFile(pipe,packet.data(),DWORD(packet.size()),&done,nullptr)&&done==packet.size()&&ReadFile(pipe,response,sizeof(response),&done,nullptr)&&done>=sizeof(Response)){
        Response reply;memcpy(&reply,response,sizeof(reply));
        if(reply.magic==0x574d4350&&reply.version==1&&sizeof(reply)+2ull*reply.message_chars==done){
            result.error=reply.error;result.changes=reply.changes;result.message.assign(reinterpret_cast<wchar_t*>(response+sizeof(reply)),reply.message_chars);
        }
        DWORD ack=0x41434b31;WriteFile(pipe,&ack,sizeof(ack),&done,nullptr);
    }
    CloseHandle(pipe);return result;
}
static bool stale_directory(HANDLE h,bool relative){
    HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");IO_STATUS_BLOCK io{};io.Information=~ULONG_PTR(0);
    if(relative){
        using OpenFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
        FARPROC proc=GetProcAddress(ntdll,"NtOpenFile");OpenFn open=nullptr;memcpy(&open,&proc,sizeof(open));
        wchar_t name[]=L"README.TXT";UNICODE_STRING us{sizeof(name)-2,sizeof(name),name};OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.RootDirectory=h;oa.ObjectName=&us;oa.Attributes=OBJ_CASE_INSENSITIVE;HANDLE file=nullptr;
        NTSTATUS status=open(&file,GENERIC_READ|SYNCHRONIZE,&oa,&io,FILE_SHARE_READ,0x60);if(file)CloseHandle(file);
        return status==NTSTATUS(0x8000001cL)&&!io.Information;
    }
    using QueryFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
    FARPROC proc=GetProcAddress(ntdll,"NtQueryDirectoryFile");QueryFn query=nullptr;memcpy(&query,&proc,sizeof(query));BYTE data[1024];
    return query(h,nullptr,nullptr,nullptr,&io,data,sizeof(data),FileDirectoryInformation,FALSE,nullptr,TRUE)==NTSTATUS(0x8000001cL)&&!io.Information;
}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 2;
    if(argc==2&&std::wstring(argv[1])==L"--hold"){Sleep(20000);return 0;}
    if(argc==3&&std::wstring(argv[1])==L"--current")return current(argv[2][0])?0:1;
    if(argc==2&&std::wstring(argv[1])==L"--initial"){
        HANDLE h=raw();test(current(L'A')&&h!=INVALID_HANDLE_VALUE&&counter(h,0),"new session restores configured disc A, not last live selection");if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return failures?1:0;
    }
    if(argc==6&&std::wstring(argv[1])==L"--held"){
        HANDLE f=file(),r=raw(),d=directory(),inherited=reinterpret_cast<HANDLE>(ULONG_PTR(_wcstoui64(argv[4],nullptr,10)));
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]),go=OpenEventW(SYNCHRONIZE,FALSE,argv[3]);
        if(f==INVALID_HANDLE_VALUE||r==INVALID_HANDLE_VALUE||d==INVALID_HANDLE_VALUE||!ready||!go)return 3;
        SetEvent(ready);if(WaitForSingleObject(go,30000)!=WAIT_OBJECT_0)return 4;
        HANDLE input=CreateFileW(argv[5],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);char text[64]{};DWORD got=0;
        if(input!=INVALID_HANDLE_VALUE){ReadFile(input,text,63,&got,nullptr);CloseHandle(input);}
        HANDLE duplicated=reinterpret_cast<HANDLE>(ULONG_PTR(_strtoui64(text,nullptr,10)));
        test(stale(f)&&stale(r)&&stale(inherited)&&stale(duplicated),"old, inherited and cross-process duplicated handles remain tied to disc A");
        test(stale_directory(d,false),"child old directory reports media changed");
        test(counter(r,2)&&current(L'B'),"existing child refreshes new B view while old raw controls observe change count");
        for(HANDLE h:{f,r,d,ready,go,duplicated})if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        return failures?1:0;
    }
    if(argc!=8)return 2;
    const wchar_t *other=argv[1],*loader=argv[2];std::wstring a=argv[3],b=argv[4],c=argv[5],bad=argv[6];bool exhaust=std::wstring(argv[7])==L"exhaust";
    HANDLE old=file(),device=raw(),dir=directory(),duplicate=nullptr;if(old==INVALID_HANDLE_VALUE||device==INVALID_HANDLE_VALUE||dir==INVALID_HANDLE_VALUE)return 3;
    test(current(L'A')&&counter(device,0),"disc A starts with exact files, geometry, TOC and PCM");
    test(CopyFileW((environment(L"WINEDOWS_PREFIX")+L"\\.winedows\\media-session.control").c_str(),L"C:\\tests\\expired-controller.bin",FALSE)!=FALSE,"save transient endpoint for stale-session rejection fixture");
    std::wstring old_path=final_path(old);SetHandleInformation(old,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
    test(DuplicateHandle(GetCurrentProcess(),old,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"duplicate old file before replacement");
    HANDLE mapped=CreateFileMappingW(old,nullptr,PAGE_READONLY,0,0,nullptr);const void *view=mapped?MapViewOfFile(mapped,FILE_MAP_READ,0,0,22):nullptr;
    std::wstring token=L"Local\\Winedows.Swap.Probe."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64()),ready_name=token+L".ready",go_name=token+L".go";
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,ready_name.c_str()),go=CreateEventW(nullptr,TRUE,FALSE,go_name.c_str());
    Child held=spawn(other,L"--held "+quote(ready_name)+L" "+quote(go_name)+L" "+std::to_wstring(ULONG_PTR(old))+L" C:\\tests\\duplicated-handle.txt");
    test(held.process&&WaitForSingleObject(ready,15000)==WAIT_OBJECT_0,"opposite-bitness child opens A before swap");
    HANDLE remote=nullptr;bool duplicated=held.process&&DuplicateHandle(GetCurrentProcess(),old,held.process,&remote,0,FALSE,DUPLICATE_SAME_ACCESS);
    HANDLE number=CreateFileW(L"C:\\tests\\duplicated-handle.txt",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);std::string numeric=std::to_string(ULONG_PTR(remote));DWORD done=0;
    if(number!=INVALID_HANDLE_VALUE){WriteFile(number,numeric.data(),DWORD(numeric.size()),&done,nullptr);CloseHandle(number);}test(duplicated&&done==numeric.size(),"transfer an old handle into the running opposite-bitness child");
    test(rpc(99).error==ERROR_INVALID_DATA&&rpc(1,b,true).error==ERROR_INVALID_DATA&&rpc(1,b,false,true).error==ERROR_INVALID_DATA&&counter(device,0),"malformed, wrong-prefix and unknown RPC requests preserve current media");
    test(cli(loader,L"swap D "+quote(bad),false)&&counter(device,0)&&current(L'A'),"invalid image preparation leaves A and its change counter intact");
    test(lock(device,true)&&cli(loader,L"swap D "+quote(b),false,"locked")&&counter(device,0)&&lock(device,false),"removal lock rejects live replacement without publishing");
    test(cli(loader,L"swap D "+quote(b))&&counter(device,2)&&current(L'B'),"CLI atomically replaces running A with complete B descriptor");
    test(stale(old)&&stale(duplicate)&&stale(device),"old file, duplicate and raw reads return MEDIA_CHANGED");
    LARGE_INTEGER size{};test(!GetFileSizeEx(old,&size)&&GetLastError()==ERROR_MEDIA_CHANGED,"old file metadata does not silently become B metadata");
    test(stale_directory(dir,false)&&stale_directory(dir,true),"old directory enumeration and relative opens report MEDIA_CHANGED");
    HANDLE stale_open=CreateFileW(old_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(stale_open==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_MEDIA_CHANGED&&GetFileAttributesW(old_path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_MEDIA_CHANGED,"absolute old-cache opens and attributes also reject stale media");if(stale_open!=INVALID_HANDLE_VALUE)CloseHandle(stale_open);
    HANDLE section=CreateFileMappingW(old,nullptr,PAGE_READONLY,0,0,nullptr);test(!section&&GetLastError()==ERROR_MEDIA_CHANGED,"old file cannot create a new mapped section");if(section)CloseHandle(section);
    test(view&&!memcmp(view,"Hello from Winedows!\r\n",22),"existing mapped A bytes remain explicitly preserved");
    CDROM_TOC toc{};done=~0u;test(!DeviceIoControl(device,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&done,nullptr)&&GetLastError()==ERROR_MEDIA_CHANGED&&!done,"old raw data-bearing IOCTL requires reopen after replacement");
    SetEvent(go);DWORD child_code=finish(held);if(child_code)printf("Held child: %s\n",log_text(held).c_str());test(child_code==0,"running opposite-bitness child sees coherent old and new generations");
    Child fresh=spawn(other,L"--current B");test(finish(fresh)==0,"child launched after swap starts on B despite unchanged drives.ini");
    HANDLE b_file=file();test(cli(loader,L"swap D "+quote(c))&&counter(device,4)&&current(L'C')&&stale(b_file),"CUE-to-ISO swap clears audio and invalidates B handles");
    test(cli(loader,L"swap D "+quote(a))&&counter(device,6)&&current(L'A')&&stale(old),"returning to A creates a fresh generation and never revives old A handles");
    HANDLE new_a=file();test(!old_path.empty()&&final_path(new_a)!=old_path,"replacement A uses a distinct immutable backing path");
    test(cli(loader,L"eject D")&&stale(new_a,ERROR_NOT_READY)&&stale(old,ERROR_NOT_READY),"controller eject applies no-media status to current and retired handles");
    test(cli(loader,L"load D")&&counter(device,8)&&read(new_a,"Hello from Winedows!\r\n")&&stale(old),"controller reload restores current A while retired A stays stale");
    CloseHandle(old);old=INVALID_HANDLE_VALUE;bool reuse_ok=true;
    for(unsigned n=0;n<64&&reuse_ok;n++){HANDLE next=file();reuse_ok=next!=INVALID_HANDLE_VALUE&&read(next,"Hello from Winedows!\r\n");if(next!=INVALID_HANDLE_VALUE)CloseHandle(next);}
    test(reuse_ok,"closed and reused numeric handles cannot inherit stale medium identity");
    if(exhaust){
        unsigned successes=0;Reply reply;
        for(unsigned n=0;n<130;n++){reply=rpc(1,c);if(reply.error)break;++successes;}
        DWORD stable=count_now(device);Reply again=rpc(1,b);
        test(successes>0&&successes<127&&reply.error==ERROR_INVALID_DATA&&reply.message.find(L"history limit")!=std::wstring::npos&&again.error==ERROR_INVALID_DATA&&count_now(device)==stable&&current(L'C'),"bounded descriptor history exhaustion preserves the last complete medium");
    }
    if(view)UnmapViewOfFile(view);
    if(mapped)CloseHandle(mapped);
    for(HANDLE h:{old,device,dir,duplicate,ready,go,b_file,new_a})if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    printf("Media swap probe: %d failures\n",failures);return failures?1:0;
}
