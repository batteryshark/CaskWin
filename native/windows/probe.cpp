// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winternl.h>
#include <winioctl.h>
#include <cstdio>
#include <string>
static int failures;
static void test(bool ok,const char *name) {printf("%s %s (error=%lu)\n",ok?"PASS":"FAIL",name,GetLastError());if(!ok)++failures;}
static bool exists(const wchar_t *p){return GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES;}
static bool write(const wchar_t *path,const char *data) {
    HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;BOOL ok=WriteFile(h,data,(DWORD)strlen(data),&n,nullptr);CloseHandle(h);return ok&&n==strlen(data);
}
static bool child(const wchar_t *exe,const wchar_t *args) {
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};std::wstring cmd=L"\""+std::wstring(exe)+L"\" "+args;
    if(!CreateProcessW(exe,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))return false;
    DWORD w=WaitForSingleObject(pi.hProcess,30000),exit=1;if(w==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&exit);else TerminateProcess(pi.hProcess,99);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return exit==0;
}
static DWORD WINAPI small_stack(LPVOID) {return write(L"C:\\wd-tests\\small-stack.txt","small stack")?0:1;}
static bool child_variant(const wchar_t *exe,bool ansi_mode) {
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    std::wstring cmd=L"\""+std::wstring(exe)+L"\" --custom-child";
    wchar_t block[]=L"WD_CUSTOM=retained\0\0";BOOL ok;
    if(ansi_mode) {
        std::string narrow(cmd.size()*4+1,0);WideCharToMultiByte(CP_ACP,0,cmd.c_str(),-1,narrow.data(),(int)narrow.size(),nullptr,nullptr);
        char env[]="WD_CUSTOM=retained\0\0";STARTUPINFOA sa{};sa.cb=sizeof(sa);
        ok=CreateProcessA(nullptr,narrow.data(),nullptr,nullptr,FALSE,0,env,nullptr,&sa,&pi);
    } else ok=CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,block,nullptr,&si,&pi);
    if(!ok)return false;DWORD w=WaitForSingleObject(pi.hProcess,30000),code=1;
    if(w==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);else TerminateProcess(pi.hProcess,99);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0;
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(!GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll")){fputs("Run this fixture through the Winedows loader.\n",stderr);return 2;}
    if(argc>1 && !wcscmp(argv[1],L"--empty")) {
        HKEY k=nullptr;LSTATUS e=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\WinedowsTests",0,KEY_READ,&k);if(k)RegCloseKey(k);
        return e==ERROR_FILE_NOT_FOUND&&!exists(L"C:\\wd-tests\\child.txt")?0:1;
    }
    if(argc>1 && !wcscmp(argv[1],L"--custom-child")) {wchar_t v[32];return GetEnvironmentVariableW(L"WD_CUSTOM",v,32)&&!wcscmp(v,L"retained")&&write(L"C:\\wd-tests\\custom-child.txt","custom")?0:1;}
    if(argc>1 && !wcscmp(argv[1],L"--persist")) {
        HKEY k=nullptr;DWORD v=0,n=sizeof(v);if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\WinedowsTests",0,KEY_READ,&k))return 1;
        LSTATUS e=RegQueryValueExW(k,L"Value",nullptr,nullptr,(BYTE*)&v,&n);RegCloseKey(k);return !e&&v==0x12345678?0:1;
    }
    if(argc>1 && !wcscmp(argv[1],L"--child")) {
        HKEY k=nullptr;DWORD v=0,n=sizeof(v);
        bool ok=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\WinedowsTests",0,KEY_READ,&k)==0;
        if(ok){ok=RegQueryValueExW(k,L"Value",nullptr,nullptr,(BYTE*)&v,&n)==0&&v==0x12345678;RegCloseKey(k);}
        return ok&&write(L"C:\\wd-tests\\child.txt","child")?0:1;
    }
    test(CreateDirectoryW(L"C:\\wd-tests",nullptr)||GetLastError()==ERROR_ALREADY_EXISTS,"create virtual C directory");
    HANDLE thread=CreateThread(nullptr,64*1024,small_stack,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);DWORD thread_code=1;
    if(thread){WaitForSingleObject(thread,30000);GetExitCodeThread(thread,&thread_code);CloseHandle(thread);}test(thread&&thread_code==0,"64 KiB stack thread initialization and I/O");
    HCRYPTPROV provider=0;BOOL acquired=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT);test(acquired!=FALSE,"legacy CryptoAPI provider for Mono");if(acquired){BYTE random[16];test(CryptGenRandom(provider,sizeof(random),random)!=FALSE,"native secure random source");CryptReleaseContext(provider,0);}
    test(write(L"C:\\wd-tests\\file.txt","prefix data"),"absolute C write");
    test(exists(L"C:\\wd-tests\\file.txt"),"virtual attributes");
    HANDLE h=CreateFileW(L"C:\\wd-tests\\file.txt",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    char data[64]{};DWORD read=0;bool ok=h!=INVALID_HANDLE_VALUE&&ReadFile(h,data,63,&read,nullptr)&&std::string(data)=="prefix data";if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);test(ok,"read virtual file");
    test(MoveFileExW(L"C:\\wd-tests\\file.txt",L"C:\\wd-tests\\renamed.txt",MOVEFILE_REPLACE_EXISTING)!=FALSE,"rename virtual destination");
    test(exists(L"C:\\wd-tests\\renamed.txt")&&!exists(L"C:\\wd-tests\\file.txt"),"rename visible");
    test(DeleteFileW(L"C:\\wd-tests\\renamed.txt")!=FALSE,"delete virtual file");
    HKEY k=nullptr;LSTATUS e=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\WinedowsTests",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr);test(e==0,"private HKCU create");
    if(!e){DWORD v=0x12345678;test(RegSetValueExW(k,L"Value",0,REG_DWORD,(BYTE*)&v,sizeof(v))==0,"private HKCU value");const wchar_t s[]=L"Unicode Ω 日本語 \\";test(RegSetValueExW(k,L"Unicode",0,REG_SZ,(BYTE*)s,sizeof(s))==0,"unicode registry value");RegCloseKey(k);}
    e=RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"Software\\WinedowsTests",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr);test(e==0,"private HKLM without elevation");if(!e){DWORD v=42;RegSetValueExW(k,L"MachineValue",0,REG_DWORD,(BYTE*)&v,4);RegCloseKey(k);}
    wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);test(child(self,L"--child"),"child inherits prefix and registry");test(exists(L"C:\\wd-tests\\child.txt"),"child file redirected");
    test(child_variant(self,false),"Unicode custom environment and null application child");
    test(child_variant(self,true),"ANSI custom environment and null application child");
    if(argc>2 && !wcscmp(argv[1],L"--cross"))test(child(argv[2],L"--child"),"cross-bitness child prefix");
    PWSTR doc=nullptr;HRESULT hr=SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&doc);test(SUCCEEDED(hr)&&doc&&wcsstr(doc,L"C:\\users\\user")==doc,"known folders use prefix");if(doc)CoTaskMemFree(doc);
    if(GetDriveTypeW(L"D:\\")==DRIVE_CDROM) {
        test(GetDriveTypeA("D:\\")==DRIVE_CDROM,"CD-ROM drive type ANSI");test((GetLogicalDrives()&(1<<3))!=0,"virtual CD enumerated");
        wchar_t label[256],fs[32];DWORD serial,flags,maxcomp;
        test(GetVolumeInformationW(L"D:\\",label,256,&serial,&maxcomp,&flags,fs,32)&&!wcscmp(fs,L"CDFS")&&(flags&FILE_READ_ONLY_VOLUME),"CD-ROM volume metadata");
        test(exists(L"D:\\disc.txt"),"virtual CD read");test(!write(L"D:\\should-not-exist.txt","forbidden")&&GetLastError()==ERROR_WRITE_PROTECT,"CD-ROM writes rejected");
        h=CreateFileW(L"D:\\disc.txt",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE) {
            LARGE_INTEGER offset{};offset.QuadPart=8;char bytes[5]{};DWORD count=0;
            test(SetFilePointerEx(h,offset,nullptr,FILE_BEGIN)&&ReadFile(h,bytes,4,&count,nullptr)&&count==4&&!memcmp(bytes,"disc",4),"read-only CD stream seek and read");
            test(GetVolumeInformationByHandleW(h,label,256,&serial,&maxcomp,&flags,fs,32)&&!wcscmp(label,L"TEST_DISC")&&!wcscmp(fs,L"CDFS")&&(flags&FILE_READ_ONLY_VOLUME),"volume metadata by CD file handle");
            CloseHandle(h);
        }else test(false,"open CD stream");
        test(!DeleteFileW(L"D:\\disc.txt")&&exists(L"D:\\disc.txt"),"CD file deletion rejected");
        test(!MoveFileExW(L"C:\\wd-tests\\child.txt",L"D:\\renamed.txt",MOVEFILE_COPY_ALLOWED)&&exists(L"C:\\wd-tests\\child.txt"),"move into read-only CD rejected");
        h=CreateFileW(L"\\\\.\\D:",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        test(h!=INVALID_HANDLE_VALUE,"virtual CD device open");
        if(h!=INVALID_HANDLE_VALUE) {
            DWORD count=0;STORAGE_DEVICE_NUMBER number{};
            test(DeviceIoControl(h,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&count,nullptr)&&number.DeviceType==FILE_DEVICE_CD_ROM,"virtual CD device identity");
            test(DeviceIoControl(h,IOCTL_STORAGE_CHECK_VERIFY2,nullptr,0,nullptr,0,&count,nullptr)!=FALSE,"virtual CD media verification");
            test(!DeviceIoControl(h,IOCTL_DISK_IS_WRITABLE,nullptr,0,nullptr,0,&count,nullptr)&&GetLastError()==ERROR_WRITE_PROTECT,"CD device write protection");
            using Query=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,int);
            auto query=(Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryVolumeInformationFile");
            IO_STATUS_BLOCK io{};DWORD device[2]{};
            test(query&&query(h,&io,device,sizeof(device),4)>=0&&device[0]==FILE_DEVICE_CD_ROM&&(device[1]&3)==3,"NT CD device metadata");
            CloseHandle(h);
        }
    }
    if(GetDriveTypeW(L"E:\\")==DRIVE_REMOVABLE){test(write(L"E:\\saved.txt","removable"),"private removable drive write");test(DeleteFileW(L"E:\\saved.txt")!=FALSE,"private removable drive delete");}
    printf("Probe %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
