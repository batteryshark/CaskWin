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
static HANDLE volume(wchar_t drive){wchar_t path[]=L"\\\\.\\D:";path[4]=drive;return CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);}
static bool command(HANDLE h,DWORD code,DWORD expected=0){DWORD done=0xcccccccc;BOOL ok=DeviceIoControl(h,code,nullptr,0,nullptr,0,&done,nullptr);return !done&&(expected?(!ok&&GetLastError()==expected):ok!=FALSE);}
static bool verify(HANDLE h,DWORD &counter){DWORD done=0;return DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&counter,sizeof(counter),&done,nullptr)&&done==sizeof(counter);}
static bool removal(HANDLE h,bool lock){PREVENT_MEDIA_REMOVAL request{};request.PreventMediaRemoval=lock;DWORD done=0xcccccccc;return DeviceIoControl(h,IOCTL_STORAGE_MEDIA_REMOVAL,&request,sizeof(request),nullptr,0,&done,nullptr)&&!done;}
static std::wstring quote(const std::wstring &s){return L"\""+s+L"\"";}
static HANDLE child(const wchar_t *exe,const std::wstring &arguments,bool unicode){
    std::wstring line=quote(exe)+L" "+arguments;
    // The shim must replace the fake identity and provide its required prefix
    // variables even when the caller supplies a completely custom environment.
    wchar_t wide[]=L"WINEDOWS_MEDIA_SESSION=Local\\Winedows.Media.00000000000000000000000000000000\0MEDIA_PROBE=unicode\0";
    char narrow[]="WINEDOWS_MEDIA_SESSION=Local\\Winedows.Media.00000000000000000000000000000000\0MEDIA_PROBE=ansi\0";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe,line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|(unicode?CREATE_UNICODE_ENVIRONMENT:0),
                       unicode?static_cast<void*>(wide):static_cast<void*>(narrow),nullptr,&startup,&pi))return nullptr;
    CloseHandle(pi.hThread);return pi.hProcess;
}
static bool finish(HANDLE process,DWORD expected=0){
    if(!process)return false;
    bool ok=WaitForSingleObject(process,15000)==WAIT_OBJECT_0;DWORD code=~0u;
    if(ok)ok=GetExitCodeProcess(process,&code)&&code==expected;
    else TerminateProcess(process,ERROR_TIMEOUT);
    CloseHandle(process);return ok;
}
static bool invoke(const wchar_t *exe,const std::wstring &args,bool unicode=true,DWORD expected=0){return finish(child(exe,args,unicode),expected);}
static bool file_read(HANDLE file,const char *expected,DWORD length){
    LARGE_INTEGER zero{};char bytes[64]{};DWORD got=0;
    return SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)&&ReadFile(file,bytes,length,&got,nullptr)&&got==length&&!memcmp(bytes,expected,length);
}
static bool raw_audio(HANDLE h,std::vector<BYTE> &pcm){
    RAW_READ_INFO request{};request.DiskOffset.QuadPart=69*2048;request.SectorCount=1;request.TrackMode=CDDA;DWORD done=0;
    pcm.resize(2352);return DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&request,sizeof(request),pcm.data(),DWORD(pcm.size()),&done,nullptr)&&done==pcm.size();
}
static bool empty_directory(HANDLE directory,bool extended=false){
    BYTE data[1024]{};IO_STATUS_BLOCK status{};status.Information=~ULONG_PTR(0);
    if(extended){
        using QueryFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,ULONG,PUNICODE_STRING);
        FARPROC address=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryDirectoryFileEx");QueryFn query=nullptr;memcpy(&query,&address,sizeof(query));
        return query&&query(directory,nullptr,nullptr,nullptr,&status,data,sizeof(data),FileDirectoryInformation,1,nullptr)==NTSTATUS(0xc0000013L)&&status.Information==0;
    }
    using QueryFn=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
    FARPROC address=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryDirectoryFile");QueryFn query=nullptr;memcpy(&query,&address,sizeof(query));
    if(!query)return false;
    return query(directory,nullptr,nullptr,nullptr,&status,data,sizeof(data),FileDirectoryInformation,FALSE,nullptr,TRUE)==NTSTATUS(0xc0000013L)&&status.Information==0;
}
static bool absent_section_ex(HANDLE file){
    using SectionFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PLARGE_INTEGER,ULONG,ULONG,HANDLE,PVOID,ULONG);
    FARPROC address=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateSectionEx");SectionFn create=nullptr;memcpy(&create,&address,sizeof(create));
    if(!create)return false;
    HANDLE section=nullptr;NTSTATUS status=create(&section,SECTION_MAP_READ,nullptr,nullptr,PAGE_READONLY,SEC_COMMIT,file,nullptr,0);
    if(section)CloseHandle(section);
    return status==NTSTATUS(0xc0000013L);
}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 2;
    if(argc>=2&&std::wstring(argv[1])==L"--child"){
        if(argc!=5)return 2;
        HANDLE h=volume(argv[3][0]);if(h==INVALID_HANDLE_VALUE)return 3;
        std::wstring op=argv[2];DWORD expected=wcstoul(argv[4],nullptr,10);bool ok=false;
        if(op==L"eject")ok=command(h,IOCTL_STORAGE_EJECT_MEDIA,expected);
        else if(op==L"load")ok=command(h,IOCTL_STORAGE_LOAD_MEDIA2,expected);
        else if(op==L"unlock")ok=removal(h,false);
        else if(op==L"cycle"){
            ok=true;for(unsigned i=0;i<25&&ok;i++)ok=command(h,IOCTL_STORAGE_EJECT_MEDIA)&&command(h,IOCTL_STORAGE_LOAD_MEDIA);
        }
        CloseHandle(h);return ok?0:4;
    }
    if(argc==2&&std::wstring(argv[1])==L"--initial"){
        HANDLE h=volume(L'D');DWORD count=~0u;test(h!=INVALID_HANDLE_VALUE&&verify(h,count)&&!count,"new loader session resets media present with change count zero");
        if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
        return failures?1:0;
    }
    if(argc!=3)return 2;
    const wchar_t *other=argv[1],*loader=argv[2];
    HANDLE d=volume(L'D'),e=volume(L'E');if(d==INVALID_HANDLE_VALUE||e==INVALID_HANDLE_VALUE)return 3;
    DWORD count=~0u,done=0;test(verify(d,count)&&!count,"CHECK_VERIFY returns initial ULONG change count");
    test(command(d,IOCTL_STORAGE_CHECK_VERIFY2),"CHECK_VERIFY2 without output succeeds");
    DWORD sentinel=0xcccccccc;done=~0u;
    test(!DeviceIoControl(d,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&sentinel,3,&done,nullptr)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER&&!done&&sentinel==0xcccccccc,"short verify buffer rejected without partial output");
    BYTE invalid=1;done=~0u;
    test(!DeviceIoControl(d,IOCTL_STORAGE_EJECT_MEDIA,&invalid,1,nullptr,0,&done,nullptr)&&GetLastError()==ERROR_INVALID_PARAMETER&&!done,"malformed eject leaves state unchanged");
    OVERLAPPED overlapped{};done=~0u;
    test(!DeviceIoControl(d,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0,&done,&overlapped)&&GetLastError()==ERROR_NOT_SUPPORTED&&!done,"unsupported overlapped eject leaves state unchanged");
    test(command(d,IOCTL_STORAGE_EJECTION_CONTROL,ERROR_NOT_SUPPORTED),"per-caller ejection locking explicitly unsupported");
    test(verify(d,count)&&!count,"rejected operations do not increment change count");
    wchar_t session_name[256]{};DWORD session_length=GetEnvironmentVariableW(L"WINEDOWS_MEDIA_SESSION",session_name,256);
    bool clobbered=session_length&&session_length<256&&SetEnvironmentVariableW(L"WINEDOWS_MEDIA_SESSION",L"replaced-by-application");
    bool propagated=clobbered&&invoke(other,L"--child load D 0",false);
    if(clobbered)SetEnvironmentVariableW(L"WINEDOWS_MEDIA_SESSION",session_name);
    test(propagated,"child uses the active session even when parent environment key was overwritten");
    HANDLE file=CreateFileW(L"D:\\README.TXT",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE directory=CreateFileW(L"D:\\",FILE_LIST_DIRECTORY|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    HANDLE writable=CreateFileW(L"E:\\state.txt",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    test(file!=INVALID_HANDLE_VALUE&&directory!=INVALID_HANDLE_VALUE&&writable!=INVALID_HANDLE_VALUE,"open optical file/directory and writable removable file before eject");
    test(file_read(file,"Hello from Winedows!\r\n",22),"initial optical file bytes");
    HANDLE cached_mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    const void *cached_view=cached_mapping?MapViewOfFile(cached_mapping,FILE_MAP_READ,0,0,22):nullptr;
    test(cached_view&&!memcmp(cached_view,"Hello from Winedows!\r\n",22),"create a mapped view before eject");
    std::vector<BYTE> original_audio;test(raw_audio(d,original_audio),"initial CDDA raw bytes available");
    test(removal(d,true)&&removal(d,true),"removal lock is a counted virtual-device property");
    test(invoke(other,L"--child eject D 170",false),"opposite-bitness child sees removal lock through ANSI custom environment");
    test(invoke(other,L"--child unlock D 0")&&invoke(other,L"--child eject D 170"),"another process can decrement one global removal lock");
    test(invoke(other,L"--child unlock D 0")&&invoke(other,L"--child eject D 0",false),"child eject becomes visible after final removal lock releases");
    test(command(d,IOCTL_STORAGE_CHECK_VERIFY,ERROR_NOT_READY),"existing raw handle reports no medium");
    test(verify(e,count)&&!count,"other virtual drive remains present");
    test(GetDriveTypeW(L"D:\\")==DRIVE_CDROM&&(GetLogicalDrives()&(1u<<3)),"empty optical device retains type and logical drive bit");
    HANDLE empty=volume(L'D');test(empty!=INVALID_HANDLE_VALUE,"bare raw device can reopen while empty for reload");if(empty!=INVALID_HANDLE_VALUE)CloseHandle(empty);
    HANDLE absent=CreateFileW(L"D:\\README.TXT",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(absent==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_NOT_READY,"new optical file opens fail while ejected");if(absent!=INVALID_HANDLE_VALUE)CloseHandle(absent);
    test(GetFileAttributesW(L"D:\\README.TXT")==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_NOT_READY,"attributes report no medium");
    char bytes[32]{};done=~0u;
    test(!ReadFile(file,bytes,1,&done,nullptr)&&GetLastError()==ERROR_NOT_READY&&!done,"already open optical file cannot read ejected cache");
    done=~0u;test(!ReadFile(d,bytes,1,&done,nullptr)&&GetLastError()==ERROR_NOT_READY&&!done,"already open raw stream cannot read ejected cache");
    test(empty_directory(directory),"already open directory cannot enumerate ejected cache");
    test(empty_directory(directory,true),"extended directory enumeration also reports no medium");
    LARGE_INTEGER file_size{};test(!GetFileSizeEx(file,&file_size)&&GetLastError()==ERROR_NOT_READY,"existing file metadata reports no medium");
    HANDLE absent_mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    test(!absent_mapping&&GetLastError()==ERROR_NOT_READY,"new mapped section from old file handle is blocked while ejected");if(absent_mapping)CloseHandle(absent_mapping);
    test(absent_section_ex(file),"NtCreateSectionEx is also blocked for ejected media");
    test(cached_view&&!memcmp(cached_view,"Hello from Winedows!\r\n",22),"existing mapped pages remain readable as explicitly documented");
    wchar_t label[32];ULARGE_INTEGER space{};
    test(!GetVolumeInformationW(L"D:\\",label,32,nullptr,nullptr,nullptr,nullptr,0)&&GetLastError()==ERROR_NOT_READY,"volume information reports no medium");
    test(!GetDiskFreeSpaceExW(L"D:\\",nullptr,&space,nullptr)&&GetLastError()==ERROR_NOT_READY,"free-space query reports no medium");
    CDROM_TOC toc{};done=~0u;test(!DeviceIoControl(d,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&done,nullptr)&&GetLastError()==ERROR_NOT_READY&&!done,"CDDA TOC unavailable while ejected");
    std::vector<BYTE> unloaded;test(!raw_audio(d,unloaded)&&GetLastError()==ERROR_NOT_READY,"CDDA PCM unavailable while ejected");
    STORAGE_DEVICE_NUMBER device{};test(DeviceIoControl(d,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&device,sizeof(device),&done,nullptr)&&device.DeviceNumber==3,"empty device identity remains available");
    done=~0u;test(!DeviceIoControl(d,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,nullptr,sizeof(device),&done,nullptr)&&GetLastError()==ERROR_INVALID_PARAMETER&&!done,"empty device identity rejects null output buffer");
    test(invoke(other,L"--child load D 0"),"opposite-bitness child reloads the mounted medium");
    test(verify(d,count)&&count==2,"eject and reload each increment media-change count exactly once");
    test(file_read(file,"Hello from Winedows!\r\n",22),"same immutable medium becomes readable through existing file handle");
    CDROM_SUB_Q_DATA_FORMAT format{};format.Format=IOCTL_CDROM_CURRENT_POSITION;SUB_Q_CHANNEL_DATA q{};
    test(DeviceIoControl(d,IOCTL_CDROM_READ_Q_CHANNEL,&format,sizeof(format),&q,sizeof(q),&done,nullptr)&&q.CurrentPosition.TrackNumber==1&&q.CurrentPosition.AbsoluteAddress[1]==0&&q.CurrentPosition.AbsoluteAddress[2]==2&&q.CurrentPosition.AbsoluteAddress[3]==0,"CDDA read cursor resets when medium generation changes");
    std::vector<BYTE> loaded;test(raw_audio(d,loaded)&&loaded==original_audio,"reloaded CDDA sectors match the original bytes");
    test(command(d,IOCTL_STORAGE_LOAD_MEDIA)&&verify(d,count)&&count==2,"reloading an already present medium is a no-op");
    constexpr DWORD cd_eject=CTL_CODE(FILE_DEVICE_CD_ROM,0x0202,METHOD_BUFFERED,FILE_READ_ACCESS);
    constexpr DWORD cd_load=CTL_CODE(FILE_DEVICE_CD_ROM,0x0203,METHOD_BUFFERED,FILE_READ_ACCESS);
    test(command(d,cd_eject)&&command(d,cd_load)&&verify(d,count)&&count==4,"legacy CDROM eject/load aliases share the same state");
    test(file_read(writable,"removable state fixture",23),"removable file starts intact");
    test(command(e,IOCTL_DISK_EJECT_MEDIA),"writable removable medium ejects virtually");
    done=~0u;test(!WriteFile(writable,"X",1,&done,nullptr)&&GetLastError()==ERROR_NOT_READY&&!done,"already open removable file cannot write while ejected");
    test(!FlushFileBuffers(writable)&&GetLastError()==ERROR_NOT_READY,"already open removable file cannot flush while ejected");
    FILE_END_OF_FILE_INFO eof{};test(!SetFileInformationByHandle(writable,FileEndOfFileInfo,&eof,sizeof(eof))&&GetLastError()==ERROR_NOT_READY,"already open removable file cannot truncate while ejected");
    absent=CreateFileW(L"E:\\state.txt",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
    test(absent==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_NOT_READY,"ejected removable file cannot be truncated by a new open");if(absent!=INVALID_HANDLE_VALUE)CloseHandle(absent);
    test(command(e,IOCTL_DISK_LOAD_MEDIA)&&file_read(writable,"removable state fixture",23)&&verify(e,count)&&count==2,"removable reload preserves bytes and has independent change counter");
    for(wchar_t letter:{L'G',L'H',L'I',L'J'}){
        if(letter==L'J'&&!(GetLogicalDrives()&(1u<<9)))continue;
        HANDLE alias=volume(letter);test(alias!=INVALID_HANDLE_VALUE&&command(alias,IOCTL_STORAGE_EJECT_MEDIA,ERROR_NOT_SUPPORTED),"aliased or nested backing roots explicitly reject ambiguous eject");if(alias!=INVALID_HANDLE_VALUE)CloseHandle(alias);
    }
    wchar_t current[32768];GetModuleFileNameW(nullptr,current,32768);DWORD before=0;verify(d,before);
    HANDLE first=child(current,L"--child cycle D 0",true),second=child(other,L"--child cycle D 0",false);
    bool first_ok=finish(first),second_ok=finish(second);
    test(first_ok&&second_ok&&verify(d,count)&&count>before&&count<=before+100&&!(count&1),"concurrent x86/x64 media transitions preserve atomic presence and counter");
    wchar_t prefix_root[32768]{};GetEnvironmentVariableW(L"WINEDOWS_PREFIX",prefix_root,32768);
    std::wstring lock_path=std::wstring(prefix_root)+L"\\.winedows\\session.lock";
    HANDLE lock=CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    test(lock==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"loader session lock remains exclusively held during media operations");
    if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
    test(invoke(loader,L"drive list",true,1),"active prefix continues to reject a nested loader");
    test(command(d,IOCTL_STORAGE_EJECT_MEDIA),"last eject stays active until this loader session ends");
    if(cached_view)UnmapViewOfFile(cached_view);
    if(cached_mapping)CloseHandle(cached_mapping);
    CloseHandle(d);CloseHandle(e);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);if(writable!=INVALID_HANDLE_VALUE)CloseHandle(writable);
    printf("Media state probe: %d failures\n",failures);return failures?1:0;
}
