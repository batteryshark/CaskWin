// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <string>
#include <vector>

static std::wstring directory() {
    std::vector<wchar_t> path(32768);
    DWORD size=GetModuleFileNameW(nullptr,path.data(),(DWORD)path.size());
    if(!size||size>=path.size())return {};
    std::wstring result(path.data(),size);result.resize(result.find_last_of(L"\\/")+1);return result;
}
static bool inspect_alias(const std::wstring &path) {
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE){printf("OPEN error=%lu\n",GetLastError());return false;}
    bool ok=true;std::vector<wchar_t> name(32768);
    for(DWORD flags:{FILE_NAME_OPENED,FILE_NAME_NORMALIZED}){
        DWORD count=GetFinalPathNameByHandleW(file,name.data(),(DWORD)name.size(),flags|VOLUME_NAME_DOS);
        if(count&&count<name.size())printf("PATH %s=%ls\n",flags==FILE_NAME_OPENED?"opened":"normalized",name.data());
        else{printf("PATH error=%lu\n",GetLastError());ok=false;}
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if(GetFileInformationByHandle(file,&information))printf("FILE links=%lu index=%08lx:%08lx\n",information.nNumberOfLinks,information.nFileIndexHigh,information.nFileIndexLow);
    else ok=false;
    CloseHandle(file);return ok;
}
using Section=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PLARGE_INTEGER,ULONG,ULONG,HANDLE);
using Duplicate=NTSTATUS(NTAPI*)(HANDLE,HANDLE,HANDLE,PHANDLE,ACCESS_MASK,ULONG,ULONG);
static bool section(HANDLE file,ULONG flags,bool blocked,const char *label) {
    auto create=(Section)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateSection");
    HANDLE mapping=nullptr;NTSTATUS status=create(&mapping,SECTION_QUERY|SECTION_MAP_READ,nullptr,nullptr,PAGE_READONLY,flags,file);
    bool ok=blocked?status==(NTSTATUS)0xc0000135:status>=0&&mapping;
    printf("%s SECTION %s flags=%08lx status=%08lx\n",ok?"PASS":"FAIL",label,flags,(ULONG)status);
    if(status>=0&&mapping)CloseHandle(mapping);
    return ok;
}
static HANDLE open_read(const std::wstring &path) {
    return CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
}
static bool reuse_allowed(const std::wstring &folder,HANDLE old) {
    std::vector<HANDLE> opened;bool seen=false,ok=true;
    // Hold other allocations so the allocator can reach a recently freed slot.
    // A passing result requires observing the exact old numeric handle again.
    for(unsigned i=0;i<1024;i++){
        HANDLE file=open_read(folder+L"allowed-target.dll");if(file==INVALID_HANDLE_VALUE){ok=false;break;}
        opened.push_back(file);
        if(file==old){seen=true;ok=section(file,SEC_IMAGE,false,"reused allowed-target handle");break;}
    }
    printf("%s REUSE observed=%d allocations=%zu\n",seen&&ok?"PASS":"FAIL",seen,opened.size());
    for(auto file:opened)CloseHandle(file);
    return seen&&ok;
}
static bool sections(const std::wstring &folder,bool blocked) {
    bool ok=true;HANDLE file=open_read(folder+L"override-marker.dll");if(file==INVALID_HANDLE_VALUE)return false;
    ok=section(file,SEC_IMAGE,blocked,"original alias")&&ok;
    ok=section(file,SEC_IMAGE_NO_EXECUTE,false,"nonexecutable alias image")&&ok;
    ok=section(file,SEC_COMMIT,false,"alias data mapping")&&ok;
    HANDLE duplicate=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)){CloseHandle(file);return false;}
    CloseHandle(file);ok=section(duplicate,SEC_IMAGE,blocked,"duplicate after original close")&&ok;
    CloseHandle(duplicate);ok=reuse_allowed(folder,file)&&ok;
    file=open_read(folder+L"override-marker.dll");if(file==INVALID_HANDLE_VALUE)return false;
    duplicate=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE)){CloseHandle(file);return false;}
    DWORD flags=0;bool source_closed=!GetHandleInformation(file,&flags),same_slot=duplicate==file;
    // Native Windows may close the original and immediately allocate that
    // numeric slot for the destination. It is then valid under its new lifetime.
    bool source_ok=source_closed||same_slot;
    printf("%s DUPLICATE_CLOSE_SOURCE sourceClosed=%d destinationReusesSlot=%d\n",source_ok?"PASS":"FAIL",source_closed,same_slot);ok=source_ok&&ok;
    ok=section(duplicate,SEC_IMAGE,blocked,"duplicate-close-source")&&ok;CloseHandle(duplicate);ok=reuse_allowed(folder,file)&&ok;
    auto duplicate_native=(Duplicate)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtDuplicateObject");
    for(bool protect:{false,true}){
        file=open_read(folder+L"override-marker.dll");if(file==INVALID_HANDLE_VALUE)return false;
        if(protect&&!SetHandleInformation(file,HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)){CloseHandle(file);return false;}
        duplicate=nullptr;NTSTATUS status=duplicate_native(GetCurrentProcess(),file,(HANDLE)(ULONG_PTR)0x12345678,&duplicate,0,0,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE);
        DWORD actual_flags=0;bool valid=GetHandleInformation(file,&actual_flags)!=FALSE;
        // Native Windows determines whether a failed close-source operation
        // consumes the handle. The runner compares this exact signature with
        // its native-policy control instead of assuming undocumented behavior.
        printf("DUPLICATE_FAIL protected=%d status=%08lx sourceValid=%d flags=%08lx\n",protect,(ULONG)status,valid,valid?actual_flags:0);
        if(status>=0){ok=false;if(duplicate)CloseHandle(duplicate);}
        if(valid){ok=section(file,SEC_IMAGE,blocked,"source after failed duplication")&&ok;SetHandleInformation(file,HANDLE_FLAG_PROTECT_FROM_CLOSE,0);CloseHandle(file);}
        ok=reuse_allowed(folder,file)&&ok;
    }
    return ok;
}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc==5&&!wcscmp(argv[1],L"--make-link")){
        BOOL ok=FALSE;DWORD error=ERROR_SUCCESS;
        if(!wcscmp(argv[2],L"symlink")){
            ok=CreateSymbolicLinkW(argv[3],argv[4],SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
            error=GetLastError();
            if(!ok&&error==ERROR_INVALID_PARAMETER){ok=CreateSymbolicLinkW(argv[3],argv[4],0);error=GetLastError();}
        }else if(!wcscmp(argv[2],L"hardlink")){ok=CreateHardLinkW(argv[3],argv[4],nullptr);error=GetLastError();}
        else return 2;
        printf("LINK kind=%ls created=%d error=%lu\n",argv[2],!!ok,ok?0:error);
        return ok?0:77;
    }
    if(argc!=3||(wcscmp(argv[1],L"--probe")&&wcscmp(argv[1],L"--sections"))||(wcscmp(argv[2],L"allowed")&&wcscmp(argv[2],L"blocked")))return 2;
    bool blocked=!wcscmp(argv[2],L"blocked");auto folder=directory();if(folder.empty())return 1;
    if(!inspect_alias(folder+L"override-marker.dll"))return 1;
    if(!wcscmp(argv[1],L"--sections"))return sections(folder,blocked)?0:1;
    if(GetModuleHandleW(L"override-marker.dll")||GetModuleHandleW(L"allowed-target.dll")){
        puts("FAIL alias/target unexpectedly loaded before dynamic parent request");return 1;
    }
    SetLastError(0);HMODULE module=LoadLibraryW((folder+L"override-bridge.dll").c_str());DWORD error=GetLastError();
    printf("DYNAMIC parent=%p error=%lu expected=%s\n",module,error,blocked?"blocked":"allowed");
    bool ok;
    if(blocked)ok=!module&&error==ERROR_MOD_NOT_FOUND;
    else{
        using Value=DWORD(WINAPI*)();auto value=module?(Value)GetProcAddress(module,"BridgeValue"):nullptr;
        ok=value&&value()==0x4e415449;
        if(module){std::vector<wchar_t> path(32768);GetModuleFileNameW(module,path.data(),(DWORD)path.size());printf("MODULE parent=%ls\n",path.data());}
    }
    if(module)FreeLibrary(module);
    printf("%s transitive alias %zu-bit\n",ok?"PASS":"FAIL",sizeof(void*)*8);return ok?0:1;
}
