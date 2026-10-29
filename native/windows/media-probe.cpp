// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void test(bool ok,const char *name) {
    printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;
}
static bool contents(const wchar_t *path,const char *expected,size_t length) {
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    std::vector<char> data(length+1);DWORD count=0;
    bool ok=ReadFile(h,data.data(),(DWORD)data.size(),&count,nullptr)&&count==length&&!memcmp(data.data(),expected,length);
    CloseHandle(h);return ok;
}
static bool protected_open(const wchar_t *path,DWORD disposition) {
    HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,disposition,0,nullptr);
    if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);return false;}
    return GetLastError()==ERROR_WRITE_PROTECT;
}
static int folder_alias() {
    HANDLE image=CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE folder=CreateFileW(L"\\\\.\\E:",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    STORAGE_DEVICE_NUMBER d{},e{};DWORD got=0;
    test(image!=INVALID_HANDLE_VALUE&&folder!=INVALID_HANDLE_VALUE&&
         DeviceIoControl(image,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&d,sizeof(d),&got,nullptr)&&
         DeviceIoControl(folder,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&e,sizeof(e),&got,nullptr)&&d.DeviceNumber==3&&e.DeviceNumber==4,
         "folder alias of mounted ISO retains a distinct raw device");
    wchar_t label[64]{};DWORD serial=0,flags=0,maxcomp=0;
    test(folder!=INVALID_HANDLE_VALUE&&GetVolumeInformationByHandleW(folder,label,64,&serial,&maxcomp,&flags,nullptr,0)&&!wcscmp(label,L"ISO_FILES_ALIAS"),
         "ISO folder alias reports its own raw volume label");
    LARGE_INTEGER at;at.QuadPart=16*2048;BYTE sector[2048]{};
    test(image!=INVALID_HANDLE_VALUE&&SetFilePointerEx(image,at,nullptr,FILE_BEGIN)&&ReadFile(image,sector,sizeof(sector),&got,nullptr)&&got==sizeof(sector)&&!memcmp(sector+1,"CD001",5),
         "image drive keeps real ISO sector reads when its files are aliased");
    if(image!=INVALID_HANDLE_VALUE)CloseHandle(image);
    if(folder!=INVALID_HANDLE_VALUE)CloseHandle(folder);
    return failures?1:0;
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc==2&&!wcscmp(argv[1],L"--alias-only"))return folder_alias();
    if(argc<7||!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")){
        fputs("Run through Winedows with LABEL SERIAL BYTES CACHE IMAGE PRIMARY|JOLIET\n",stderr);return 2;
    }
    DWORD expected_serial=wcstoul(argv[2],nullptr,10);ULONGLONG expected_bytes=_wcstoui64(argv[3],nullptr,10);
    bool joliet=!wcscmp(argv[6],L"JOLIET");
    test(GetDriveTypeW(L"D:\\")==DRIVE_CDROM&&GetDriveTypeA("D:\\")==DRIVE_CDROM,"ISO drive identifies as CD-ROM in W/A APIs");
    test((GetLogicalDrives()&(1u<<3))!=0,"ISO drive enumerated in prefix");
    wchar_t label[128]{},fs[32]{};DWORD serial=0,flags=0,maxcomp=0;
    test(GetVolumeInformationW(L"D:\\",label,128,&serial,&maxcomp,&flags,fs,32)&&
         !wcscmp(label,argv[1])&&serial==expected_serial&&!wcscmp(fs,L"CDFS")&&(flags&FILE_READ_ONLY_VOLUME),
         "image label, Wine serial, CDFS, and read-only metadata");
    ULARGE_INTEGER available{},total{},free_bytes{};
    test(GetDiskFreeSpaceExW(L"D:\\",&available,&total,&free_bytes)&&total.QuadPart==expected_bytes&&!available.QuadPart&&!free_bytes.QuadPart,
         "disc capacity comes from ISO rather than cache host disk");
    DWORD sectors=0,bytes=0,free_clusters=1,clusters=0;
    test(GetDiskFreeSpaceW(L"D:\\",&sectors,&bytes,&free_clusters,&clusters)&&bytes==2048&&!free_clusters&&
         ULONGLONG(sectors)*bytes*clusters==expected_bytes,"ISO logical sectors and capacity");
    test(contents(L"D:\\Readme.txt","Hello from Winedows!\r\n",22),"primary/Joliet file read and version stripping");
    test(contents(L"D:\\DATA\\Nested.txt","nested ISO data\r\n",17),"nested directory read");
    test(contents(L"D:\\Empty","",0),"empty file and terminal dot normalization");
    if(joliet)test(contents(L"D:\\R\u00e9sum\u00e9 \u03a9.txt","Joliet names\n",13),"Joliet UCS-2 filename read");
    HANDLE h=CreateFileW(L"D:\\Payload.bin",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    bool seek_ok=false;
    if(h!=INVALID_HANDLE_VALUE){
        LARGE_INTEGER at;at.QuadPart=2037;BYTE data[2057];DWORD got=0;
        seek_ok=SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,data,sizeof(data),&got,nullptr)&&got==sizeof(data);
        for(size_t n=0;seek_ok&&n<sizeof(data);n++)seek_ok=data[n]==BYTE(((2037+n)*17+3)%256);
        test(GetVolumeInformationByHandleW(h,label,128,&serial,&maxcomp,&flags,fs,32)&&serial==expected_serial&&
             !wcscmp(label,argv[1])&&!wcscmp(fs,L"CDFS"),"disc file handle keeps image volume metadata");
        CloseHandle(h);
    }
    test(seek_ok,"filesystem seek/read crosses sector boundaries");
    WIN32_FIND_DATAW found{};h=FindFirstFileW(L"D:\\*",&found);unsigned count=0;
    if(h!=INVALID_HANDLE_VALUE){do{if(wcscmp(found.cFileName,L".")&&wcscmp(found.cFileName,L".."))++count;}while(FindNextFileW(h,&found));FindClose(h);}
    test(count==(joliet?5u:4u),"disc root directory enumeration");
    test(protected_open(L"D:\\new.txt",CREATE_NEW),"new CD file rejected");
    test(protected_open(L"D:\\Readme.txt",OPEN_EXISTING),"existing CD file write rejected");
    test(!DeleteFileW(L"D:\\Readme.txt"),"CD file deletion rejected");
    test(!MoveFileExW(L"D:\\Readme.txt",L"D:\\changed.txt",0),"CD file rename rejected");
    test(!CreateDirectoryW(L"D:\\new-directory",nullptr),"CD directory creation rejected");
    test(!SetFileAttributesW(L"D:\\Readme.txt",FILE_ATTRIBUTE_NORMAL),"CD attribute mutation rejected");
    std::wstring cache_file=std::wstring(argv[4])+L"\\Readme.txt",cache_new=std::wstring(argv[4])+L"\\host-write.txt";
    test(protected_open(cache_file.c_str(),OPEN_EXISTING)&&protected_open(cache_new.c_str(),CREATE_NEW),
         "host cache paths retain read-only policy inside prefix");
    test(protected_open(argv[5],OPEN_EXISTING),"retained image cannot be written by host path");
    h=CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    test(h!=INVALID_HANDLE_VALUE,"raw virtual CD device opens without physical device mount");
    if(h!=INVALID_HANDLE_VALUE){
        LARGE_INTEGER at;at.QuadPart=16*2048;BYTE data[2048]{};DWORD got=0;
        test(SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,data,sizeof(data),&got,nullptr)&&
             got==sizeof(data)&&data[0]==1&&!memcmp(data+1,"CD001",5),"raw PVD sector comes from retained ISO image");
        at.QuadPart=30*2048+2037;BYTE payload[2057]{};
        bool ok=SetFilePointerEx(h,at,nullptr,FILE_BEGIN)&&ReadFile(h,payload,sizeof(payload),&got,nullptr)&&got==sizeof(payload);
        for(size_t n=0;ok&&n<sizeof(payload);n++)ok=payload[n]==BYTE(((2037+n)*17+3)%256);
        test(ok,"raw image seek/read returns real sectors across boundaries");
        test(GetVolumeInformationByHandleW(h,label,128,&serial,&maxcomp,&flags,fs,32)&&serial==expected_serial&&
             !wcscmp(label,argv[1])&&!wcscmp(fs,L"CDFS"),"raw image handle maps back to CD volume metadata");
        DWORD done=0;STORAGE_DEVICE_NUMBER device{};
        test(DeviceIoControl(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&device,sizeof(device),&done,nullptr)&&
             device.DeviceType==FILE_DEVICE_CD_ROM&&device.DeviceNumber==3,"raw handle identifies virtual optical drive");
        test(!DeviceIoControl(h,IOCTL_DISK_IS_WRITABLE,nullptr,0,nullptr,0,&done,nullptr)&&GetLastError()==ERROR_WRITE_PROTECT,
             "raw image reports read-only media");
        CloseHandle(h);
    }
    test(protected_open(L"\\\\.\\D:",OPEN_EXISTING),"raw volume write open rejected");
    test(contents(L"D:\\Readme.txt","Hello from Winedows!\r\n",22),"disc file unchanged after rejected operations");
    printf("Media probe: %d failures\n",failures);return failures?1:0;
}
