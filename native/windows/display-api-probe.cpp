// SPDX-License-Identifier: LGPL-2.1-or-later
// Read-only display queries. Guard pages bound every caller-owned output buffer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
static std::string active_case="startup";
static DWORD query_index=ENUM_CURRENT_SETTINGS;
static LONG WINAPI crash(EXCEPTION_POINTERS *exception){
    auto record=exception->ExceptionRecord;
    printf("FAULT case=%s code=%08lx address=%p operation=%llu target=%p\n",active_case.c_str(),record->ExceptionCode,record->ExceptionAddress,
            record->NumberParameters?static_cast<unsigned long long>(record->ExceptionInformation[0]):0,
            record->NumberParameters>1?reinterpret_cast<void*>(record->ExceptionInformation[1]):nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
struct Buffer {
    BYTE *allocation=nullptr,*output=nullptr;size_t capacity=0,canary=0;
    Buffer(size_t size,bool sentinel):capacity(size),canary(sentinel?32:0){
        SYSTEM_INFO system{};GetSystemInfo(&system);size_t page=system.dwPageSize;
        allocation=static_cast<BYTE*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS));
        require(allocation!=nullptr,"allocate bounded display buffer");DWORD old;
        require(VirtualProtect(allocation+page,page,PAGE_READWRITE,&old)!=FALSE,"commit middle display page");
        output=allocation+page*2-size-canary;memset(output,0xcc,size+canary);
    }
    ~Buffer(){if(allocation)VirtualFree(allocation,0,MEM_RELEASE);}
    void check(){for(size_t i=0;i<canary;i++)require(output[capacity+i]==0xcc,"output crossed requested legacy/full capacity");}
};
template<typename Mode> static void query(bool simple,WORD input_size,bool sentinel,DWORD width,DWORD height){
    constexpr bool wide=sizeof(Mode)==sizeof(DEVMODEW);
    const WORD legacy=static_cast<WORD>(offsetof(Mode,dmICMMethod));
    const WORD expected=simple?legacy:input_size;
    std::string label=std::string(simple?"simple":"extended")+(wide?"-W":"-A")+"-input"+std::to_string(input_size)+(sentinel?"-canary":"-guard");
    active_case=label;Buffer buffer(expected,sentinel);Mode *mode=reinterpret_cast<Mode*>(buffer.output);
    mode->dmSize=input_size;mode->dmDriverExtra=simple?0x55aa:0;
    printf("BEGIN case=%s capacity=%u input_extra=%u\n",active_case.c_str(),expected,mode->dmDriverExtra);
    BOOL ok;
    if constexpr(wide)ok=simple?EnumDisplaySettingsW(nullptr,query_index,mode):EnumDisplaySettingsExW(nullptr,query_index,mode,0);
    else ok=simple?EnumDisplaySettingsA(nullptr,query_index,mode):EnumDisplaySettingsExA(nullptr,query_index,mode,0);
    DWORD error=GetLastError();buffer.check();require(ok!=FALSE,"display query succeeds");
    printf("RESULT case=%s size=%u extra=%u width=%lu height=%lu bpp=%lu fields=%08lx error=%lu\n",active_case.c_str(),mode->dmSize,mode->dmDriverExtra,mode->dmPelsWidth,mode->dmPelsHeight,mode->dmBitsPerPel,mode->dmFields,error);
    require(mode->dmSize==expected,"reported size preserves native legacy/full contract");
    require(mode->dmDriverExtra==0,"simple call clears driver extra and extended call preserves zero");
    require(mode->dmPelsWidth&&mode->dmPelsHeight,"query reports positive dimensions");
    if(width&&height)require(mode->dmPelsWidth==width&&mode->dmPelsHeight==height,"query reports expected physical/virtual dimensions");
    printf("PASS %s\n",active_case.c_str());active_case="between-cases";
}
int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetUnhandledExceptionFilter(crash);setvbuf(stdout,nullptr,_IONBF,0);
    DWORD width=0,height=0;
    for(int i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--expect-size")&&i+2<argc){width=wcstoul(argv[++i],nullptr,10);height=wcstoul(argv[++i],nullptr,10);if(!width||!height)return 2;}
        else if(!wcscmp(argv[i],L"--mode")&&i+1<argc){
            const wchar_t *name=argv[++i];
            if(!wcscmp(name,L"current"))query_index=ENUM_CURRENT_SETTINGS;
            else if(!wcscmp(name,L"registry"))query_index=ENUM_REGISTRY_SETTINGS;
            else if(!wcscmp(name,L"first"))query_index=0;
            else return 2;
        }else {puts("display-api-probe [--mode current|registry|first] [--expect-size width height]");return 2;}
    }
    printf("MODE index=%lu\n",query_index);
    try{
        static_assert(offsetof(DEVMODEA,dmICMMethod)==124&&offsetof(DEVMODEW,dmICMMethod)==188,"Windows legacy display mode layouts");
        static_assert(sizeof(DEVMODEA)==156&&sizeof(DEVMODEW)==220,"Windows full display mode layouts");
        for(bool sentinel:{false,true}){
            for(WORD size:{WORD(0),WORD(1),WORD(sizeof(DEVMODEA))})query<DEVMODEA>(true,size,sentinel,width,height);
            for(WORD size:{WORD(0),WORD(1),WORD(sizeof(DEVMODEW))})query<DEVMODEW>(true,size,sentinel,width,height);
            for(WORD size:{WORD(offsetof(DEVMODEA,dmICMMethod)),WORD(sizeof(DEVMODEA))})query<DEVMODEA>(false,size,sentinel,width,height);
            for(WORD size:{WORD(offsetof(DEVMODEW,dmICMMethod)),WORD(sizeof(DEVMODEW))})query<DEVMODEW>(false,size,sentinel,width,height);
        }
        puts("PASS display API buffer contract cases=20");return 0;
    }catch(const std::exception &error){printf("FAIL case=%s reason=%s\n",active_case.c_str(),error.what());return 1;}
}
