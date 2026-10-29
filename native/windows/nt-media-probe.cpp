// SPDX-License-Identifier: LGPL-2.1-or-later
// Direct NT virtual-media contract. Generated CUE media only; no host disk IOCTLs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Ioctl=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,ULONG,PVOID,ULONG,PVOID,ULONG);
using Create=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
using Query=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
using Read=NTSTATUS(NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG);
static Ioctl device_io;
static Create create_file;
static Query query_file;
static Read read_file;
static int failures;
static LONG apcs;
constexpr NTSTATUS ok=0,unsupported=NTSTATUS(0xc00000bb),small=NTSTATUS(0xc0000023),invalid=NTSTATUS(0xc000000d),
    absent=NTSTATUS(0xc0000013),busy=NTSTATUS(0x80000011),denied=NTSTATUS(0xc0000022),fault=NTSTATUS(0xc0000005);
constexpr DWORD last_error_marker=0x5a17c0de;
static void test(bool value,const char *name){printf("%s %s\n",value?"PASS":"FAIL",name);if(!value)++failures;}
template<class T>static T proc(const char *name){FARPROC address=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),name);T result=nullptr;static_assert(sizeof(result)==sizeof(address));memcpy(&result,&address,sizeof(result));return result;}
static void NTAPI apc(PVOID,PIO_STATUS_BLOCK,ULONG){InterlockedIncrement(&apcs);}
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;explicit Handle(HANDLE value=INVALID_HANDLE_VALUE):h(value){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}operator HANDLE()const{return h;}};
static HANDLE open_nt(const wchar_t *path,ULONG mode,ACCESS_MASK access=FILE_GENERIC_READ){
    std::wstring name=path;UNICODE_STRING us{USHORT(name.size()*2),USHORT(name.size()*2),name.data()};
    OBJECT_ATTRIBUTES oa{};oa.Length=sizeof(oa);oa.ObjectName=&us;oa.Attributes=OBJ_CASE_INSENSITIVE;
    HANDLE handle=INVALID_HANDLE_VALUE;IO_STATUS_BLOCK io{};
    NTSTATUS status=create_file(&handle,access|SYNCHRONIZE,&oa,&io,nullptr,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,1,mode|0x40,nullptr,0);
    if(status<0){printf("OPEN status=%08lx mode=%lx\n",ULONG(status),mode);return INVALID_HANDLE_VALUE;}return handle;
}
static bool request(HANDLE handle,ULONG code,void *input,ULONG input_size,void *output,ULONG output_size,NTSTATUS expected,ULONG_PTR bytes,const char *name,HANDLE event=nullptr,PIO_APC_ROUTINE callback=nullptr,void *context=nullptr){
    IO_STATUS_BLOCK io{};io.Status=NTSTATUS(0x12345678);io.Information=~ULONG_PTR(0);
    ULONGLONG started=GetTickCount64();SetLastError(last_error_marker);
    NTSTATUS status=device_io(handle,event,callback,context,&io,code,input,input_size,output,output_size);
    DWORD error=GetLastError();ULONGLONG duration=GetTickCount64()-started;
    bool passed=status==expected&&io.Status==expected&&io.Information==bytes&&error==last_error_marker&&duration<2000;
    printf("NT status=%08lx iosb=%08lx bytes=%llu lastError=%08lx duration=%llums\n",ULONG(status),ULONG(io.Status),(unsigned long long)io.Information,error,(unsigned long long)duration);
    test(passed,name);return passed;
}
static bool command(HANDLE h,ULONG code,NTSTATUS expected,const char *name){return request(h,code,nullptr,0,nullptr,0,expected,0,name);}
static bool verify(HANDLE h,DWORD expected,const char *name){DWORD counter=~0u;bool passed=request(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&counter,sizeof(counter),ok,sizeof(counter),name);test(counter==expected,"media-change count matches expected value");return passed&&counter==expected;}
static bool removal(HANDLE h,bool locked,const char *name){PREVENT_MEDIA_REMOVAL value{};value.PreventMediaRemoval=locked;return request(h,IOCTL_STORAGE_MEDIA_REMOVAL,&value,sizeof(value),nullptr,0,ok,0,name);}
static DWORD frame(const BYTE *p){return DWORD(p[1])*60*75+DWORD(p[2])*75+p[3];}
static bool cue_pcm(const BYTE *data,unsigned first,unsigned sectors){
    for(unsigned sector=0;sector<sectors;sector++)for(unsigned sample=0;sample<588;sample++){
        short l=short((first+sector)*64+sample),r=short(-l);size_t offset=sector*2352+sample*4;
        if(data[offset]!=BYTE(l)||data[offset+1]!=BYTE(USHORT(l)>>8)||data[offset+2]!=BYTE(r)||data[offset+3]!=BYTE(USHORT(r)>>8))return false;
    }return true;
}
static void toc(HANDLE h){
    std::vector<BYTE> buffer(sizeof(CDROM_TOC)+16,0xcc);
    request(h,IOCTL_CDROM_READ_TOC,nullptr,0,buffer.data(),ULONG(buffer.size()),ok,36,"NT TOC returns exact bounded byte count");
    CDROM_TOC expected{};expected.Length[1]=34;expected.FirstTrack=1;expected.LastTrack=3;
    DWORD addresses[]={150,219,238,256};
    for(unsigned i=0;i<4;i++){auto &t=expected.TrackData[i];t.Adr=1;t.Control=i?0:4;t.TrackNumber=i==3?0xaa:BYTE(i+1);t.Address[1]=BYTE(addresses[i]/4500);t.Address[2]=BYTE(addresses[i]/75%60);t.Address[3]=BYTE(addresses[i]%75);}
    test(!memcmp(buffer.data(),&expected,36)&&std::all_of(buffer.begin()+36,buffer.end(),[](BYTE b){return b==0xcc;}),"NT TOC matches complete generated CUE layout and preserves output tail");
}
static DWORD position(HANDLE h){
    CDROM_SUB_Q_DATA_FORMAT format{};format.Format=IOCTL_CDROM_CURRENT_POSITION;SUB_Q_CURRENT_POSITION value{};
    if(!request(h,IOCTL_CDROM_READ_Q_CHANNEL,&format,sizeof(format),&value,sizeof(value),ok,sizeof(value),"NT Q-channel returns exact structure"))return ~0u;
    test(value.Header.AudioStatus==AUDIO_STATUS_NO_STATUS,"Q-channel does not invent playback state");return frame(value.AbsoluteAddress);
}
static void native_baseline(const wchar_t *path){
    Handle file(CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    test(file.h!=INVALID_HANDLE_VALUE,"open ordinary fixture file for nonvirtual baseline");if(file.h==INVALID_HANDLE_VALUE)return;
    Handle port(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));test(port.h!=nullptr,"create empty native completion port");
    HANDLE associated=CreateIoCompletionPort(file,port,0x1234,1);DWORD association_error=GetLastError();
    printf("NATIVE_ASSOC associated=%d error=%lu\n",associated!=nullptr,association_error);
    test(!associated&&association_error==ERROR_INVALID_PARAMETER,"native synchronous file rejects completion-port association");
    IO_STATUS_BLOCK io{};io.Status=NTSTATUS(0x12345678);io.Information=0x5678;
    BYTE data[32];memset(data,0xcc,sizeof(data));SetLastError(last_error_marker);
    NTSTATUS result=device_io(file,nullptr,nullptr,nullptr,&io,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,data,sizeof(data));DWORD error=GetLastError();
    DWORD bytes=0;ULONG_PTR key=0;OVERLAPPED *overlap=nullptr;BOOL queued=GetQueuedCompletionStatus(port,&bytes,&key,&overlap,0);
    printf("NATIVE_IOCTL status=%08lx iosb=%08lx bytes=%llu lastError=%08lx packet=%d overlap=%d\n",ULONG(result),ULONG(io.Status),(unsigned long long)io.Information,error,queued!=FALSE,overlap!=nullptr);
    test(result==invalid&&io.Status==NTSTATUS(0x12345678)&&io.Information==0x5678&&error==last_error_marker&&!queued&&!overlap,"ordinary-file device query preserves native failure and untouched IOSB");
    LARGE_INTEGER offset{};io={};SetLastError(last_error_marker);
    result=read_file(file,nullptr,nullptr,nullptr,&io,data,1,&offset,nullptr);error=GetLastError();overlap=nullptr;
    queued=GetQueuedCompletionStatus(port,&bytes,&key,&overlap,0);
    printf("NATIVE_READ status=%08lx iosb=%08lx bytes=%llu lastError=%08lx packet=%d overlap=%d\n",ULONG(result),ULONG(io.Status),(unsigned long long)io.Information,error,queued!=FALSE,overlap!=nullptr);
    test(result==ok&&io.Status==ok&&io.Information==1&&data[0]=='N'&&error==last_error_marker&&!queued&&!overlap,"successful synchronous null-context NT read preserves native result");
    Handle asynchronous(CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    test(asynchronous.h!=INVALID_HANDLE_VALUE&&CreateIoCompletionPort(asynchronous,port,0x5678,1)==port.h,"asynchronous native file associates with completion port");
    IO_STATUS_BLOCK asynchronous_io{};BYTE byte=0;SetLastError(last_error_marker);
    result=read_file(asynchronous,nullptr,nullptr,&asynchronous_io,&asynchronous_io,&byte,1,&offset,nullptr);error=GetLastError();overlap=nullptr;
    queued=GetQueuedCompletionStatus(port,&bytes,&key,&overlap,2000);
    bool complete=queued&&key==0x5678&&bytes==1&&overlap==reinterpret_cast<OVERLAPPED*>(&asynchronous_io)&&asynchronous_io.Status==ok&&asynchronous_io.Information==1&&byte=='N';
    printf("NATIVE_ASYNC complete=%d lastError=%08lx\n",complete,error);
    test((result==ok||result==NTSTATUS(0x103))&&error==last_error_marker&&complete,"native asynchronous read supplies positive completion-port control");
}
static void suite(ULONG mode){
    printf("SYNCHRONOUS mode=%lx\n",mode);Handle h(open_nt(L"\\??\\D:",mode));
    test(h.h!=INVALID_HANDLE_VALUE,"NtCreateFile opens virtual raw device");if(h.h==INVALID_HANDLE_VALUE)return;
    IO_STATUS_BLOCK query{};ULONG actual=0;
    test(query_file(h,&query,&actual,sizeof(actual),FILE_INFORMATION_CLASS(16))==ok&&(actual&0x30)==mode,"native FileModeInformation retains requested ALERT/NONALERT semantics");
    verify(h,0,"initial NT verify succeeds");command(h,IOCTL_STORAGE_CHECK_VERIFY2,ok,"NT verify2 without output succeeds");toc(h);
    RAW_READ_INFO raw{};raw.DiskOffset.QuadPart=69*2048;raw.SectorCount=2;raw.TrackMode=CDDA;
    std::vector<BYTE> pcm(4704+16,0xcc);
    request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw),pcm.data(),4704,ok,4704,"NT raw PCM returns exactly two CDDA sectors");
    test(cue_pcm(pcm.data(),66,2)&&std::all_of(pcm.begin()+4704,pcm.end(),[](BYTE b){return b==0xcc;}),"NT raw PCM matches generated source sectors byte for byte");
    test(position(h)==221,"NT raw read advances reader cursor exactly two sectors");
    DWORD win_count=~0u,win_bytes=0;test(DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&win_count,sizeof(win_count),&win_bytes,nullptr)&&!win_count&&win_bytes==4,"Win32 sees the same NT media state");
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    request(h,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0,unsupported,0,"event form rejected before eject",event);
    request(h,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0,unsupported,0,"APC form rejected before eject",nullptr,apc);
    request(h,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0,unsupported,0,"context-only form rejected before eject",nullptr,nullptr,reinterpret_cast<void*>(1));
    request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw),pcm.data(),4704,unsupported,0,"event raw read rejected before cursor mutation",event);
    SleepEx(20,TRUE);test(!apcs&&WaitForSingleObject(event,0)==WAIT_TIMEOUT,"unsupported calls neither queue APCs nor signal event");
    Handle asynchronous(open_nt(L"\\??\\D:",0));test(asynchronous.h!=INVALID_HANDLE_VALUE,"open asynchronous virtual device handle");
    if(asynchronous.h!=INVALID_HANDLE_VALUE){
        Handle completion(CreateIoCompletionPort(asynchronous,nullptr,0x7777,1));test(completion.h!=nullptr,"asynchronous virtual handle can associate a completion port");
        command(asynchronous,IOCTL_STORAGE_EJECT_MEDIA,unsupported,"asynchronous handle rejected before eject");
        DWORD bytes=0;ULONG_PTR key=0;OVERLAPPED *packet=nullptr;BOOL queued=GetQueuedCompletionStatus(completion,&bytes,&key,&packet,0);
        test(!queued&&!packet,"rejected asynchronous virtual call queues no completion packet");
    }
    verify(h,0,"unsupported async forms preserve media generation");test(position(h)==221,"unsupported raw read preserves reader cursor");
    DWORD sentinel=0xcccccccc;
    request(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&sentinel,3,small,0,"short verify buffer rejected with zero IOSB bytes");test(sentinel==0xcccccccc,"short verify leaves output untouched");
    std::fill(pcm.begin(),pcm.end(),0xcc);request(h,IOCTL_CDROM_READ_TOC,nullptr,0,pcm.data(),4,small,0,"short TOC buffer rejected");
    test(std::all_of(pcm.begin(),pcm.end(),[](BYTE b){return b==0xcc;}),"short TOC does not partially write output");
    request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw),pcm.data(),2352,small,0,"short PCM output rejected before read");
    request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw)-1,pcm.data(),4704,invalid,0,"short RAW_READ_INFO input rejected");
    raw.DiskOffset.QuadPart++;request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw),pcm.data(),4704,invalid,0,"unaligned raw offset rejected");raw.DiskOffset.QuadPart--;
    BYTE malformed=1;request(h,IOCTL_STORAGE_EJECT_MEDIA,&malformed,1,nullptr,0,invalid,0,"malformed eject rejected before transition");
    SetLastError(last_error_marker);NTSTATUS no_iosb=device_io(h,nullptr,nullptr,nullptr,nullptr,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0);DWORD no_iosb_error=GetLastError();
    test(no_iosb==invalid&&no_iosb_error==last_error_marker,"null IOSB rejects eject before mutation");
    auto bad=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);test(bad!=nullptr,"allocate inaccessible fixture page");
    if(bad){
        request(h,IOCTL_CDROM_RAW_READ,bad,sizeof(raw),pcm.data(),4704,fault,0,"inaccessible input rejected safely");
        request(h,IOCTL_CDROM_READ_TOC,nullptr,0,bad,36,fault,0,"inaccessible output rejected safely");
        SetLastError(last_error_marker);NTSTATUS status=device_io(h,nullptr,nullptr,nullptr,(PIO_STATUS_BLOCK)bad,IOCTL_STORAGE_EJECT_MEDIA,nullptr,0,nullptr,0);DWORD saved=GetLastError();
        test(status==fault&&saved==last_error_marker,"inaccessible IOSB rejects eject before mutation");VirtualFree(bad,0,MEM_RELEASE);
    }
    auto readonly_page=(BYTE*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    test(readonly_page!=nullptr,"allocate read-only output fixture page");
    if(readonly_page){memset(readonly_page,0xcc,4096);DWORD old=0;VirtualProtect(readonly_page,4096,PAGE_READONLY,&old);
        request(h,IOCTL_CDROM_READ_TOC,nullptr,0,readonly_page,36,fault,0,"read-only output rejected safely");test(readonly_page[0]==0xcc,"read-only output remains untouched");VirtualFree(readonly_page,0,MEM_RELEASE);}
    auto guarded=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);test(guarded!=nullptr,"allocate guarded fixture page");
    if(guarded){DWORD old=0;VirtualProtect(guarded,4096,PAGE_READWRITE|PAGE_GUARD,&old);
        request(h,IOCTL_CDROM_RAW_READ,guarded,sizeof(raw),pcm.data(),4704,fault,0,"guarded input rejected without consuming guard");
        request(h,IOCTL_CDROM_READ_TOC,nullptr,0,guarded,36,fault,0,"guarded output rejected without consuming guard");
        MEMORY_BASIC_INFORMATION memory{};test(VirtualQuery(guarded,&memory,sizeof(memory))==sizeof(memory)&&(memory.Protect&PAGE_GUARD),"buffer validation preserves PAGE_GUARD protection");VirtualFree(guarded,0,MEM_RELEASE);}
    auto crossing=(BYTE*)VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);test(crossing!=nullptr,"allocate cross-page buffer fixture");
    if(crossing){memset(crossing,0xcc,4096);DWORD old=0;VirtualProtect(crossing+4096,4096,PAGE_NOACCESS,&old);
        request(h,IOCTL_CDROM_READ_TOC,nullptr,0,crossing+4096-16,36,fault,0,"output spanning inaccessible page rejected before partial write");
        request(h,IOCTL_CDROM_RAW_READ,crossing+4096-8,sizeof(raw),pcm.data(),4704,fault,0,"input spanning inaccessible page rejected safely");
        test(std::all_of(crossing+4096-16,crossing+4096,[](BYTE b){return b==0xcc;}),"cross-page output leaves accessible bytes untouched");VirtualFree(crossing,0,MEM_RELEASE);}
    Handle attributes(open_nt(L"\\??\\D:",mode,FILE_READ_ATTRIBUTES));test(attributes.h!=INVALID_HANDLE_VALUE,"open metadata-only raw handle");
    if(attributes.h!=INVALID_HANDLE_VALUE){request(attributes,IOCTL_CDROM_READ_TOC,nullptr,0,pcm.data(),ULONG(pcm.size()),denied,0,"READ_ACCESS ioctl denied without FILE_READ_DATA");command(attributes,IOCTL_STORAGE_CHECK_VERIFY2,ok,"ANY_ACCESS verify2 works with metadata-only handle");command(attributes,IOCTL_STORAGE_EJECT_MEDIA,denied,"READ_ACCESS eject denied before state mutation");}
    verify(h,0,"invalid buffers and denied operations preserve media generation");test(position(h)==221,"invalid buffers preserve reader cursor");
    removal(h,true,"first NT removal lock succeeds");removal(h,true,"second NT removal lock succeeds");command(h,IOCTL_STORAGE_EJECT_MEDIA,busy,"counted lock prevents NT eject");
    removal(h,false,"release one NT removal lock");command(h,IOCTL_STORAGE_EJECT_MEDIA,busy,"remaining lock still prevents NT eject");removal(h,false,"release final NT removal lock");
    command(h,IOCTL_STORAGE_EJECT_MEDIA,ok,"NT eject succeeds after unlock");command(h,IOCTL_STORAGE_CHECK_VERIFY,absent,"NT verify reports absent medium");
    Handle empty(open_nt(L"\\??\\D:",mode));test(empty.h!=INVALID_HANDLE_VALUE,"NtCreateFile reopens raw device while medium is absent");
    if(empty.h!=INVALID_HANDLE_VALUE)command(empty,IOCTL_STORAGE_CHECK_VERIFY,absent,"new raw handle observes absent medium");
    request(h,IOCTL_CDROM_READ_TOC,nullptr,0,pcm.data(),ULONG(pcm.size()),absent,0,"NT TOC reports absent medium");request(h,IOCTL_CDROM_RAW_READ,&raw,sizeof(raw),pcm.data(),4704,absent,0,"NT raw PCM reports absent medium");
    STORAGE_DEVICE_NUMBER number{};request(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),ok,sizeof(number),"empty device identity remains available");test(number.DeviceType==FILE_DEVICE_CD_ROM&&number.DeviceNumber==3,"empty virtual identity describes D CD-ROM");
    win_bytes=~0u;test(!DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,nullptr,0,&win_bytes,nullptr)&&GetLastError()==ERROR_NOT_READY&&!win_bytes,"Win32 observes NT eject");
    request(h,IOCTL_STORAGE_LOAD_MEDIA,nullptr,0,nullptr,0,unsupported,0,"event reload rejected while absent",event);command(h,IOCTL_STORAGE_CHECK_VERIFY,absent,"rejected reload leaves medium absent");
    command(h,IOCTL_STORAGE_LOAD_MEDIA2,ok,"NT reload succeeds");verify(h,2,"NT eject and load increment generation exactly twice");test(position(h)==150,"reload resets reader cursor to first data track");
    command(h,IOCTL_STORAGE_LOAD_MEDIA,ok,"loading present medium is a no-op");verify(h,2,"no-op reload preserves generation");toc(h);
    // Common I/O completion behavior is separately measured against native NT.
    Handle port(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));HANDLE associated=CreateIoCompletionPort(h,port,0x5678,1);DWORD association_error=GetLastError();
    test(!associated&&association_error==ERROR_INVALID_PARAMETER,"synchronous virtual device rejects IOCP association like native file");
    command(h,IOCTL_STORAGE_CHECK_VERIFY2,ok,"synchronous null-context device query still succeeds after rejected association");
    DWORD bytes=0;ULONG_PTR key=0;OVERLAPPED *overlap=nullptr;BOOL queued=GetQueuedCompletionStatus(port,&bytes,&key,&overlap,0);test(!queued&&!overlap,"synchronous null-context virtual query queues no completion packet");
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    device_io=proc<Ioctl>("NtDeviceIoControlFile");create_file=proc<Create>("NtCreateFile");query_file=proc<Query>("NtQueryInformationFile");read_file=proc<Read>("NtReadFile");if(!device_io||!create_file||!query_file||!read_file)return 2;
    if(argc==3&&!wcscmp(argv[1],L"--native")){native_baseline(argv[2]);return failures?1:0;}
    if(argc!=3||!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 2;
    ULONG mode=!wcscmp(argv[1],L"alert")?0x10:!wcscmp(argv[1],L"nonalert")?0x20:0;if(!mode)return 2;
    native_baseline(argv[2]);suite(mode);printf("NT media probe failures=%d\n",failures);return failures?1:0;
}
