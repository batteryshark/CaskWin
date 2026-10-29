// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup-imports.h"
#include "detours.h"
#include <cstring>
#include <cwchar>
static void require(bool ok,const char *name){if(!ok)throw std::runtime_error(std::string(name)+" error="+std::to_string(GetLastError()));}
static std::string ansi(const wchar_t *text){BOOL lossy=FALSE;int n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,text,-1,nullptr,0,nullptr,&lossy);std::string out(n,0);require(n&&!lossy&&WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,text,-1,out.data(),n,nullptr,&lossy)&&!lossy,"ANSI fixture path");return out;}
template<class T>static T read(HANDLE process,uintptr_t address){T value{};SIZE_T done=0;require(ReadProcessMemory(process,(void*)address,&value,sizeof(value),&done)&&done==sizeof(value),"read fixture child");return value;}
static void tamper(HANDLE process,WORD machine,bool lookup){
    // Tamper cases intentionally use same-architecture children; normal binder
    // cases below exercise both directions through its real cross-arch API.
    require((sizeof(void*)==4)==(machine==IMAGE_FILE_MACHINE_I386),"same-architecture tamper fixture");
    using Query=NTSTATUS(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);auto query=(Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION basic{};require(query(process,0,&basic,sizeof(basic),nullptr)>=0,"query fixture PEB");
    uintptr_t image=read<uintptr_t>(process,(uintptr_t)basic.PebBaseAddress+(sizeof(void*)==4?8:16));
    auto dos=read<IMAGE_DOS_HEADER>(process,image);auto nt=read<IMAGE_NT_HEADERS>(process,image+dos.e_lfanew);auto directory=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    uintptr_t begin=image+directory.VirtualAddress,end=begin+directory.Size;
    for(unsigned i=0;i<2048;i++){
        uintptr_t address=begin+i*sizeof(IMAGE_IMPORT_DESCRIPTOR);auto d=read<IMAGE_IMPORT_DESCRIPTOR>(process,address);if(!d.Name)break;
        char name[16]{};SIZE_T done=0;require(ReadProcessMemory(process,(void*)(image+d.Name),name,sizeof(name),&done)&&done==sizeof(name),"read original name");
        if(strcmp(name,"gdiplus.dll"))continue;
        require(image+d.OriginalFirstThunk<begin||image+d.OriginalFirstThunk>=end,"original lookup outside injected allocation");
        if(lookup){uintptr_t zero=0;DWORD protection=0,ignored=0;void *target=(void*)(image+d.OriginalFirstThunk);require(VirtualProtectEx(process,target,sizeof(zero),PAGE_READWRITE,&protection),"make fixture lookup writable");BOOL written=WriteProcessMemory(process,target,&zero,sizeof(zero),&done);require(VirtualProtectEx(process,target,sizeof(zero),protection,&ignored),"restore fixture lookup protection");require(written&&done==sizeof(zero),"tamper original lookup");}
        else {DWORD value=d.Name+1;require(WriteProcessMemory(process,(void*)(address+offsetof(IMAGE_IMPORT_DESCRIPTOR,Name)),&value,sizeof(value),&done)&&done==sizeof(value),"tamper original descriptor");}
        return;
    }
    throw std::runtime_error("Missing original GDI+ descriptor to tamper");
}
int wmain(int argc,wchar_t **argv){
    if(argc!=9)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
    PROCESS_INFORMATION child{};bool rejection=wcscmp(argv[8],L"bind")!=0;
    try{
        WORD machine=(WORD)wcstoul(argv[3],nullptr,16);std::wstring application=argv[2],cwd=application.substr(0,application.find_last_of(L"\\/"));
        auto plan=wd::plan_builtin_startup_imports(application,machine,cwd,cwd,cwd,{argv[4]});
        require(!plan.bindings.empty(),"actual selected static imports are planned");
        std::string helper=ansi(argv[1]),provider=ansi(argv[4]);const char *dlls[]={helper.c_str(),provider.c_str()};
        std::wstring command=L"\""+application+L"\" \""+argv[4]+L"\" \""+argv[5]+L"\" \""+argv[6]+L"\"";
        STARTUPINFOW start{};start.cb=sizeof(start);
        const wchar_t *actual=!wcscmp(argv[8],L"wrong-image")?argv[7]:application.c_str();
        require(DetourCreateProcessWithDllsW(actual,command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,cwd.c_str(),&start,&child,!wcscmp(argv[8],L"omit")?1:2,dlls,nullptr),"create suspended binding fixture");
        if(!wcscmp(argv[8],L"descriptor-change"))tamper(child.hProcess,machine,false);
        if(!wcscmp(argv[8],L"lookup-change"))tamper(child.hProcess,machine,true);
        wd::bind_builtin_startup_imports(child.hProcess,plan);
        if(rejection)throw std::runtime_error("unsafe binding unexpectedly accepted");
        require(ResumeThread(child.hThread)==1,"binder preserves suspension count");DWORD code=124;
        if(WaitForSingleObject(child.hProcess,30000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&code);else TerminateProcess(child.hProcess,124);
        CloseHandle(child.hThread);CloseHandle(child.hProcess);child={};printf("CHILD %08lx\n",code);return code?1:0;
    }catch(const std::exception &error){
        bool created=child.hProcess!=nullptr;if(created){TerminateProcess(child.hProcess,125);CloseHandle(child.hThread);CloseHandle(child.hProcess);}
        bool expected=rejection&&(created||!wcscmp(argv[8],L"missing-export"))&&strstr(error.what(),"Native startup imports:")==error.what();
        printf("%s %s\n",expected?"PASS expected binding rejection":"FAIL",error.what());return expected?0:1;
    }
}
