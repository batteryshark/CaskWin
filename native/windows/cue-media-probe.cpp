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
static DWORD frames(const BYTE *p){return p[1]*4500+p[2]*75+p[3];}
static bool expected_sector(const BYTE *pcm,DWORD disc_lba) {
    if((disc_lba>=64&&disc_lba<67)||(disc_lba>=82&&disc_lba<86)) {
        for(unsigned n=0;n<2352;n++)if(pcm[n])return false;
        return true;
    }
    DWORD source=disc_lba<82?disc_lba-3:disc_lba-7;
    for(unsigned n=0;n<588;n++) {
        short left=short(source*64+n),right=short(-left);
        if(pcm[n*4]!=BYTE(left)||pcm[n*4+1]!=BYTE(static_cast<unsigned short>(left)>>8)||
           pcm[n*4+2]!=BYTE(right)||pcm[n*4+3]!=BYTE(static_cast<unsigned short>(right)>>8))return false;
    }
    return true;
}
static bool raw(HANDLE h,DWORD lba,DWORD count) {
    RAW_READ_INFO request{};request.DiskOffset.QuadPart=LONGLONG(lba)*2048;request.SectorCount=count;request.TrackMode=CDDA;
    std::vector<BYTE> pcm(size_t(count)*2352,0xcc);DWORD got=0;
    if(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&request,sizeof(request),pcm.data(),DWORD(pcm.size()),&got,nullptr)||got!=pcm.size())return false;
    for(DWORD n=0;n<count;n++)if(!expected_sector(pcm.data()+size_t(n)*2352,lba+n))return false;
    return true;
}
static bool seek_status(HANDLE h,DWORD lba,BYTE track,BYTE index,DWORD relative) {
    DWORD f=lba+150;CDROM_SEEK_AUDIO_MSF seek{};seek.M=BYTE(f/4500);seek.S=BYTE(f/75%60);seek.F=BYTE(f%75);DWORD got=0;
    if(!DeviceIoControl(h,IOCTL_CDROM_SEEK_AUDIO_MSF,&seek,sizeof(seek),nullptr,0,&got,nullptr))return false;
    CDROM_SUB_Q_DATA_FORMAT fmt{};fmt.Format=IOCTL_CDROM_CURRENT_POSITION;SUB_Q_CHANNEL_DATA q{};
    return DeviceIoControl(h,IOCTL_CDROM_READ_Q_CHANNEL,&fmt,sizeof(fmt),&q,sizeof(q),&got,nullptr)&&
           q.CurrentPosition.TrackNumber==track&&q.CurrentPosition.IndexNumber==index&&
           frames(q.CurrentPosition.AbsoluteAddress)==f&&frames(q.CurrentPosition.TrackRelativeAddress)==relative;
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc!=7||!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll"))return 2;
    test(GetDriveTypeW(L"D:\\")==DRIVE_CDROM,"mixed-mode drive identifies as optical media");
    HANDLE file=CreateFileW(L"D:\\README.TXT",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    char data[23]{};DWORD got=0;
    test(file!=INVALID_HANDLE_VALUE&&ReadFile(file,data,22,&got,nullptr)&&got==22&&!memcmp(data,"Hello from Winedows!\r\n",22),"MODE1 data track exposes real ISO files");
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    HANDLE h=CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(h!=INVALID_HANDLE_VALUE,"open private mixed-mode raw volume");if(h==INVALID_HANDLE_VALUE)return 1;
    LARGE_INTEGER size{};test(GetFileSizeEx(h,&size)&&size.QuadPart==64*2048,"cooked image retains actual 64-sector data extent");
    ULARGE_INTEGER available{},total{},free_bytes{};
    test(GetDiskFreeSpaceExW(L"D:\\",&available,&total,&free_bytes)&&total.QuadPart==48*2048,"filesystem capacity remains its declared 48 sectors");
    LARGE_INTEGER at;at.QuadPart=16*2048;BYTE sector[2048]{};
    test(SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,sector,sizeof(sector),&got,nullptr)&&got==sizeof(sector)&&!memcmp(sector+1,"CD001",5),"raw logical PVD comes from MODE1 payload");
    at.QuadPart=52*2048;
    bool padding=SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,sector,sizeof(sector),&got,nullptr)&&got==sizeof(sector);
    for(BYTE b:sector)if(b!=(52^0xa5))padding=false;
    test(padding,"data sectors beyond filesystem length remain intact");
    CDROM_TOC toc{};
    test(DeviceIoControl(h,IOCTL_CDROM_READ_TOC,nullptr,0,&toc,sizeof(toc),&got,nullptr)&&toc.FirstTrack==1&&toc.LastTrack==3&&
         (toc.TrackData[0].Control&4)&&frames(toc.TrackData[0].Address)==150&&frames(toc.TrackData[1].Address)==219&&
         frames(toc.TrackData[2].Address)==238&&toc.TrackData[3].TrackNumber==0xaa&&frames(toc.TrackData[3].Address)==256,
         "TOC preserves CUE INDEX01 plus both kinds of pregap");
    test(raw(h,64,10),"inserted silence, stored INDEX00 and audio bytes retain exact offsets");
    test(raw(h,81,8),"raw read crosses audio and following inserted/stored gaps");
    test(raw(h,88,18),"final audio track matches source through lead-out");
    test(seek_status(h,67,2,0,2),"stored INDEX00 reports track02 index0 countdown");
    test(seek_status(h,83,3,0,5),"inserted PREGAP reports track03 index0 countdown");
    test(seek_status(h,88,3,1,0),"INDEX01 seek reports exact track03 start");
    RAW_READ_INFO request{};request.DiskOffset.QuadPart=63*2048;request.SectorCount=2;request.TrackMode=CDDA;BYTE audio[4704];memset(audio,0xcc,sizeof(audio));
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&request,sizeof(request),audio,sizeof(audio),&got,nullptr)&&GetLastError()==ERROR_INVALID_PARAMETER&&!got&&audio[0]==0xcc,
         "mixed data/audio read rejects data without fabricating PCM");
    request.DiskOffset.QuadPart=105*2048;
    test(!DeviceIoControl(h,IOCTL_CDROM_RAW_READ,&request,sizeof(request),audio,sizeof(audio),&got,nullptr)&&!got,"raw read beyond real CUE lead-out rejected");
    CloseHandle(h);
    h=CreateFileW(L"\\\\.\\D:",GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    test(h==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_WRITE_PROTECT,"mixed-mode raw volume remains read-only");
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    bool protected_cache=true;
    for(int n=1;n<argc;n++) {
        h=CreateFileW(argv[n],GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_WRITE_PROTECT)protected_cache=false;
        if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    }
    test(protected_cache,"CUE, BIN, ISO, manifest and PCM private host paths remain immutable");
    printf("CUE media probe: %d failures\n",failures);return failures?1:0;
}
