// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <cstdio>
#include <vector>
#include <algorithm>

static int failures;
static void test(bool ok,const char *name){printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static DWORD frame(const BYTE *p){return DWORD(p[1])*60*75+DWORD(p[2])*75+p[3];}
static bool known_pcm(const BYTE *data,DWORD sectors,DWORD start_sample=0) {
    for(DWORD n=0;n<sectors*588;n++) {
        short l=short(int((n+start_sample)%1000)-500),r=short(-l);
        if(data[n*4]!=BYTE(l)||data[n*4+1]!=BYTE(static_cast<unsigned short>(l)>>8)||
           data[n*4+2]!=BYTE(r)||data[n*4+3]!=BYTE(static_cast<unsigned short>(r)>>8))return false;
    }
    return true;
}
static HANDLE open_drive(wchar_t drive) {
    wchar_t path[]=L"\\\\.\\D:";path[4]=drive;
    return CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
}
static bool expected_disc(HANDLE h,wchar_t letter) {
    if(h==INVALID_HANDLE_VALUE)return false;
    DWORD got=0;CDROM_TOC toc{};STORAGE_DEVICE_NUMBER device{};
    if(!DeviceIoControl(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&device,sizeof(device),&got,nullptr)||device.DeviceNumber!=DWORD(letter-L'A'))return false;
    if(!DeviceIoControl(h,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&got,nullptr)||toc.LastTrack!=(letter==L'D'?3:2)||frame(toc.TrackData[toc.LastTrack].Address)!=(letter==L'D'?675u:450u))return false;
    wchar_t label[32]{};DWORD serial=0,flags=0,maxcomp=0;
    if(!GetVolumeInformationByHandleW(h,label,32,&serial,&maxcomp,&flags,nullptr,0)||wcscmp(label,letter==L'D'?L"D_DISC":L"E_DISC"))return false;
    RAW_READ_INFO read{};read.DiskOffset.QuadPart=150*2048;read.SectorCount=1;read.TrackMode=CDDA;BYTE pcm[2352]{};
    if(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),pcm,sizeof(pcm),&got,nullptr)||got!=sizeof(pcm))return false;
    if(letter==L'E')return known_pcm(pcm,1);
    for(BYTE b:pcm)if(b)return false;
    return true;
}
static int aliases() {
    HANDLE d=open_drive(L'D'),e=open_drive(L'E'),duplicate=nullptr;
    test(expected_disc(d,L'D')&&expected_disc(e,L'E'),"aliased folders keep distinct raw TOC, PCM, labels and device numbers");
    bool copied=e!=INVALID_HANDLE_VALUE&&DuplicateHandle(GetCurrentProcess(),e,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS);
    test(copied&&expected_disc(duplicate,L'E'),"DuplicateHandle retains raw E drive identity");
    HANDLE closed=e;if(e!=INVALID_HANDLE_VALUE)CloseHandle(e);
    test(copied&&expected_disc(duplicate,L'E'),"duplicate retains identity after original closes");
    bool reused=false,correct=true;
    for(unsigned n=0;n<64;n++) {
        wchar_t letter=n%2?L'E':L'D';HANDLE current=open_drive(letter);
        reused|=current==closed;correct&=expected_disc(current,letter);
        if(current!=INVALID_HANDLE_VALUE)CloseHandle(current);
    }
    test(correct&&(!copied||expected_disc(duplicate,L'E')),"repeated close/reopen never inherits stale raw device identity");
    printf("Raw numeric handle reuse observed: %s\n",reused?"yes":"no (identity checked for every reopen)");
    using DuplicateNt=LONG(NTAPI*)(HANDLE,HANDLE,HANDLE,PHANDLE,ACCESS_MASK,ULONG,ULONG);
    DuplicateNt nt=nullptr;FARPROC proc=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtDuplicateObject");
    static_assert(sizeof(nt)==sizeof(proc));memcpy(&nt,&proc,sizeof(nt));
    HANDLE nt_copy=nullptr;LONG status=nt&&d!=INVALID_HANDLE_VALUE?nt(GetCurrentProcess(),d,GetCurrentProcess(),&nt_copy,0,0,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE):-1;
    if(status>=0)d=INVALID_HANDLE_VALUE;
    test(status>=0&&expected_disc(nt_copy,L'D'),"NtDuplicateObject with close-source preserves raw D identity");
    if(d!=INVALID_HANDLE_VALUE)CloseHandle(d);
    if(duplicate)CloseHandle(duplicate);
    if(nt_copy)CloseHandle(nt_copy);
    printf("CDDA alias probe: %d failures\n",failures);return failures?1:0;
}
static int boundary() {
    HANDLE h=open_drive(L'F');CDROM_TOC toc{};DWORD got=0;
    test(h!=INVALID_HANDLE_VALUE&&DeviceIoControl(h,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&got,nullptr)&&
         toc.LastTrack==2&&toc.TrackData[2].Address[1]==255&&toc.TrackData[2].Address[2]==59&&toc.TrackData[2].Address[3]==74,
         "largest representable lead-out stays at 255:59:74");
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    return failures?1:0;
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc!=2||!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 2;
    if(!wcscmp(argv[1],L"--aliases"))return aliases();
    if(!wcscmp(argv[1],L"--boundary"))return boundary();
    HANDLE h=CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(h!=INVALID_HANDLE_VALUE,"open private CDDA drive without host device mount");if(h==INVALID_HANDLE_VALUE)return 1;
    CDROM_TOC toc{};DWORD got=0;
    BOOL ok=DeviceIoControl(h,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&got,nullptr);
    test(ok&&got==36&&toc.Length[0]==0&&toc.Length[1]==34&&toc.FirstTrack==1&&toc.LastTrack==3,"bounded TOC header and track count");
    test(toc.TrackData[0].TrackNumber==1&&(toc.TrackData[0].Control&4)&&frame(toc.TrackData[0].Address)==150,
         "explicit synthetic data track starts at LBA zero");
    test(toc.TrackData[1].TrackNumber==2&&!(toc.TrackData[1].Control&4)&&frame(toc.TrackData[1].Address)==300&&
         toc.TrackData[2].TrackNumber==3&&frame(toc.TrackData[2].Address)==525&&toc.TrackData[3].TrackNumber==0xaa&&frame(toc.TrackData[3].Address)==675,
         "audio track numbers, real PCM durations and lead-out");
    test(!DeviceIoControl(h,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,4,&got,nullptr)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER&&!got,"short TOC output rejected");
    RAW_READ_INFO read{};read.TrackMode=CDDA;read.DiskOffset.QuadPart=150*2048;read.SectorCount=2;
    std::vector<BYTE> bytes(2352*2,0xcc);
    ok=DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr);
    bool silent=true;for(BYTE b:bytes)if(b)silent=false;
    test(ok&&got==bytes.size()&&silent,"2352-byte sectors return prepared silent track PCM");
    read.DiskOffset.QuadPart=375*2048;
    test(DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr)&&got==bytes.size()&&known_pcm(bytes.data(),2),
         "nonzero PCM stereo samples match fixture byte for byte");
    read.DiskOffset.QuadPart=374*2048;
    ok=DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr);
    silent=true;for(size_t n=0;n<2352;n++)if(bytes[n])silent=false;
    test(ok&&silent&&known_pcm(bytes.data()+2352,1),"raw read spanning adjacent audio tracks");
    CDROM_SUB_Q_DATA_FORMAT fmt{};fmt.Format=IOCTL_CDROM_CURRENT_POSITION;SUB_Q_CHANNEL_DATA q{};
    test(DeviceIoControl(h,IOCTL_CDROM_READ_Q_CHANNEL,&fmt,sizeof(fmt),&q,sizeof(q),&got,nullptr)&&
         q.CurrentPosition.Header.AudioStatus==AUDIO_STATUS_NO_STATUS&&q.CurrentPosition.TrackNumber==3&&
         frame(q.CurrentPosition.AbsoluteAddress)==526&&frame(q.CurrentPosition.TrackRelativeAddress)==1,
         "Q channel exposes read cursor without claiming hardware playback");
    CDROM_SEEK_AUDIO_MSF seek{};seek.S=4;
    test(DeviceIoControl(h,IOCTL_CDROM_SEEK_AUDIO_MSF,&seek,sizeof(seek),nullptr,0,&got,nullptr)&&
         DeviceIoControl(h,IOCTL_CDROM_READ_Q_CHANNEL,&fmt,sizeof(fmt),&q,sizeof(q),&got,nullptr)&&
         q.CurrentPosition.TrackNumber==2&&frame(q.CurrentPosition.AbsoluteAddress)==300,
         "MSF seek selects expected audio track");
    read.DiskOffset.QuadPart=0;read.SectorCount=1;
    std::fill(bytes.begin(),bytes.end(),0xcc);
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr)&&GetLastError()==ERROR_INVALID_PARAMETER&&!got&&bytes[0]==0xcc,
         "data track is not fabricated as silent CDDA");
    read.DiskOffset.QuadPart=524*2048;read.SectorCount=2;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr)&&!got,"read beyond lead-out rejected");
    read.DiskOffset.QuadPart=375*2048+1;read.SectorCount=1;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr),"unaligned raw offset rejected");
    read.DiskOffset.QuadPart=-2048;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr),"negative raw offset rejected");
    read.DiskOffset.QuadPart=375*2048;read.SectorCount=0xffffffffu;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr),"oversized raw count rejected without overflow");
    read.SectorCount=2;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),2352,&got,nullptr)&&GetLastError()==ERROR_INSUFFICIENT_BUFFER,"short PCM output rejected");
    read.TrackMode=YellowMode2;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&read,sizeof(read),bytes.data(),(DWORD)bytes.size(),&got,nullptr),"unsupported raw sector mode rejected");
    CDROM_PLAY_AUDIO_MSF play{};play.StartingS=4;play.EndingS=5;
    test(!DeviceIoControl(h,IOCTL_CDROM_PLAY_AUDIO_MSF,&play,sizeof(play),nullptr,0,&got,nullptr)&&GetLastError()==ERROR_NOT_SUPPORTED,
         "analog play explicitly unsupported; Wine renders digital PCM");
    CloseHandle(h);
    h=CreateFileW(argv[1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_WRITE_PROTECT,"private PCM host path remains immutable to game");
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    printf("CDDA media probe: %d failures\n",failures);return failures?1:0;
}
