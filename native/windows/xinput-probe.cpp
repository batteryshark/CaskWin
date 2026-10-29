// SPDX-License-Identifier: LGPL-2.1-or-later
// Read-only controller queries: never issue valid-index SetState or Enable,
// inject input, or print button/axis/keystroke/device identity data.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <setupapi.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures;
static void check(bool ok,const char *name,DWORD status=0) {
    printf("%s %s (status=%lu)\n",ok?"PASS":"FAIL",name,status);
    if(!ok)++failures;
}
static std::wstring path(HMODULE module) {
    wchar_t value[32768];DWORD count=GetModuleFileNameW(module,value,32768);
    return count&&count<32768?std::wstring(value,count):std::wstring();
}
static bool provider(HMODULE module,const wchar_t *name,bool wine) {
    std::wstring expected;
    if(wine){
        HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
        if(!shim)return false;
        expected=path(shim);expected.resize(expected.find_last_of(L"\\/")+1);
        expected+=sizeof(void*)==8?L"builtin\\x64\\":L"builtin\\x86\\";
    }else{wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);expected=system;expected+=L"\\";}
    expected+=name;
    wprintf(L"Provider %ls: %ls\n",name,path(module).c_str());
    if(!_wcsicmp(path(module).c_str(),expected.c_str()))return true;
    // WOW64 records either the logical System32 spelling or the physical
    // SysWOW64 spelling depending on how the trusted loader opened the DLL.
    if(!wine&&sizeof(void*)==4){
        wchar_t wow[MAX_PATH]{};UINT count=GetSystemWow64DirectoryW(wow,MAX_PATH);
        if(count&&count<MAX_PATH){expected=std::wstring(wow)+L"\\"+name;return !_wcsicmp(path(module).c_str(),expected.c_str());}
    }
    return false;
}
template<class T> static T proc(HMODULE module,const char *name) {
    T result=(T)GetProcAddress(module,name);check(result!=nullptr,name,GetLastError());return result;
}
using State=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
using Caps=DWORD(WINAPI*)(DWORD,DWORD,XINPUT_CAPABILITIES*);
using SetState=DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
using Keys=DWORD(WINAPI*)(DWORD,DWORD,XINPUT_KEYSTROKE*);
using Battery=DWORD(WINAPI*)(DWORD,BYTE,XINPUT_BATTERY_INFORMATION*);
using Audio=DWORD(WINAPI*)(DWORD,WCHAR*,UINT*,WCHAR*,UINT*);
using Sound=DWORD(WINAPI*)(DWORD,GUID*,GUID*);
struct CapsEx {XINPUT_CAPABILITIES caps;WORD vendor,product,version,unknown;DWORD unknown2;};
using ExtendedCaps=DWORD(WINAPI*)(DWORD,DWORD,DWORD,CapsEx*);
template<class T> struct Guarded {DWORD before=0x123abcde;T value{};DWORD after=0xabcde123;};
template<class T> static bool guarded(const Guarded<T>&value){return value.before==0x123abcde&&value.after==0xabcde123;}

static void enumeration() {
    const GUID wine={0x6c53d5fd,0x6480,0x440f,{0xb6,0x18,0x47,0x67,0x50,0xc5,0xe1,0xa6}};
    const GUID hid={0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
    for(const GUID *guid:{&wine,&hid}){
        HDEVINFO info=SetupDiGetClassDevsW(guid,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
        check(info!=INVALID_HANDLE_VALUE,"read-only device interface enumeration",GetLastError());
        if(info==INVALID_HANDLE_VALUE)continue;
        SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);DWORD count=0;
        while(SetupDiEnumDeviceInterfaces(info,nullptr,guid,count,&item))++count;
        check(GetLastError()==ERROR_NO_MORE_ITEMS,"device interface enumeration completed",GetLastError());
        printf("INTERFACES %s=%lu (metadata only; no device opened)\n",guid==&wine?"wine_bus":"hid",count);
        SetupDiDestroyDeviceInfoList(info);
    }
}
#ifdef WINEDOWS_STATIC_XINPUT
static bool imported_from(const char *dll,const char *symbol,HMODULE module) {
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto entry=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;entry->Name;++entry)if(!_stricmp((char*)base+entry->Name,dll)){
        if(!entry->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+entry->OriginalFirstThunk);
        auto bound=(IMAGE_THUNK_DATA*)(base+entry->FirstThunk);
        for(;names->u1.AddressOfData;++names,++bound){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto name=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)name->Name,symbol))return (FARPROC)bound->u1.Function==GetProcAddress(module,symbol);
        }
    }
    return false;
}
#endif
int wmain(int argc,wchar_t **argv) {
    if(argc<2)return 2;
    if(!wcscmp(argv[1],L"--system-load-diagnostic")){
        wchar_t system[32768]{},wow[32768]{};
        UINT system_count=GetSystemDirectoryW(system,32768),wow_count=GetSystemWow64DirectoryW(wow,32768);DWORD wow_error=GetLastError();
        wprintf(L"System=%ls count=%u WOW=%ls count=%u error=%lu bits=%u\n",system,system_count,wow,wow_count,wow_error,(unsigned)(8*sizeof(void*)));
        wchar_t small_wow[MAX_PATH]{};UINT small_count=GetSystemWow64DirectoryW(small_wow,MAX_PATH);
        wprintf(L"WOW bounded=%ls count=%u error=%lu\n",small_wow,small_count,GetLastError());
        for(const wchar_t *directory:{system,wow})for(DWORD flags:{(DWORD)0,(DWORD)LOAD_LIBRARY_SEARCH_SYSTEM32,(DWORD)(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)}){
            std::wstring name=std::wstring(directory)+L"\\xinput1_4.dll";
            HMODULE loaded=LoadLibraryExW(name.c_str(),nullptr,flags);DWORD error=GetLastError();
            wprintf(L"LOAD path=%ls flags=%lx module=%p error=%lu actual=%ls\n",name.c_str(),flags,loaded,error,loaded?path(loaded).c_str():L"");
            if(loaded)FreeLibrary(loaded);
        }
        HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
        auto load_backend=shim?(HMODULE(WINAPI*)())GetProcAddress(shim,"WinedowsLoadSystemXInput"):nullptr;
        HMODULE loaded=load_backend?load_backend():nullptr;DWORD error=GetLastError();
        wprintf(L"BACKEND export=%p module=%p error=%lu actual=%ls\n",load_backend,loaded,error,loaded?path(loaded).c_str():L"");
        if(loaded)FreeLibrary(loaded);
        return 0;
    }
    const wchar_t *dll=argv[1];bool wine=true,expect_missing=false;
    for(int i=2;i<argc;++i){if(!wcscmp(argv[i],L"--native"))wine=false;if(!wcscmp(argv[i],L"--expect-missing"))expect_missing=true;}
    enumeration();
    HMODULE module=LoadLibraryW(dll);
    if(expect_missing){check(!module,"strict missing builtin rejected",GetLastError());return failures?1:0;}
    check(module!=nullptr,"load XInput provider",GetLastError());if(!module)return 1;
    check(provider(module,dll,wine),"selected provider path");
    auto state=proc<State>(module,"XInputGetState");
    auto caps=proc<Caps>(module,"XInputGetCapabilities");
    auto set=proc<SetState>(module,"XInputSetState");
    if(!state||!caps||!set)return 1;
    bool modern=!_wcsicmp(dll,L"xinput1_3.dll")||!_wcsicmp(dll,L"xinput1_4.dll");
    bool version14=!_wcsicmp(dll,L"xinput1_4.dll");
    bool version91=!_wcsicmp(dll,L"xinput9_1_0.dll");
    Keys keys=modern?proc<Keys>(module,"XInputGetKeystroke"):nullptr;
    Battery battery=modern?proc<Battery>(module,"XInputGetBatteryInformation"):nullptr;
    Audio audio=version14?proc<Audio>(module,"XInputGetAudioDeviceIds"):nullptr;
    Sound sound=!version14?proc<Sound>(module,"XInputGetDSoundAudioDeviceGuids"):nullptr;
    State extended=modern?(State)GetProcAddress(module,MAKEINTRESOURCEA(100)):nullptr;
    ExtendedCaps extended_caps=version14?(ExtendedCaps)GetProcAddress(module,MAKEINTRESOURCEA(108)):nullptr;
    if(modern)check(extended!=nullptr,"extended state ordinal 100 export");
    if(wine&&version14)check(extended_caps!=nullptr,"extended capabilities ordinal 108 export");
    if(!version91)check(GetProcAddress(module,"XInputEnable")!=nullptr,"enable export (not invoked)");
#ifdef WINEDOWS_STATIC_XINPUT
    char imported_name[80];WideCharToMultiByte(CP_ACP,0,dll,-1,imported_name,sizeof(imported_name),nullptr,nullptr);
    check(imported_from(imported_name,"XInputGetState",module),"startup state import binding");
    check(imported_from(imported_name,"XInputGetCapabilities",module),"startup capabilities import binding");
    check(imported_from(imported_name,"XInputSetState",module),"startup set-state import binding");
    Guarded<XINPUT_STATE> startup;XINPUT_CAPABILITIES startup_caps{};XINPUT_VIBRATION startup_output{};
    check(XInputGetState(4,&startup.value)==ERROR_BAD_ARGUMENTS&&guarded(startup),"startup state call contract");
    check(XInputGetCapabilities(4,0,&startup_caps)==ERROR_BAD_ARGUMENTS,"startup capabilities call contract");
    check(XInputSetState(4,&startup_output)==ERROR_BAD_ARGUMENTS,"startup invalid-index set-state (no output)");
#endif
    Guarded<XINPUT_STATE> value;Guarded<XINPUT_CAPABILITIES> capabilities;
    XINPUT_VIBRATION output{};DWORD result;
    check(state(4,&value.value)==ERROR_BAD_ARGUMENTS&&guarded(value),"out-of-range state rejected");
    check(state(0,nullptr)==ERROR_BAD_ARGUMENTS,"null state rejected");
    check(caps(4,0,&capabilities.value)==ERROR_BAD_ARGUMENTS&&guarded(capabilities),"out-of-range capabilities rejected");
    check(set(4,&output)==ERROR_BAD_ARGUMENTS,"out-of-range set-state rejected (no output)");
    if(keys){Guarded<XINPUT_KEYSTROKE> key;result=keys(4,0,&key.value);check(result==ERROR_BAD_ARGUMENTS&&guarded(key),"out-of-range keystroke rejected",result);}
    if(battery){Guarded<XINPUT_BATTERY_INFORMATION> info;result=battery(4,BATTERY_DEVTYPE_GAMEPAD,&info.value);check(result==ERROR_BAD_ARGUMENTS&&guarded(info),"out-of-range battery rejected",result);}
    unsigned connected=0;
    for(DWORD index=0;index<XUSER_MAX_COUNT;++index){
        printf("CONTROLLER slot=%lu\n",index);
        // Query keystrokes before the first valid state call to test lazy backend initialization.
        if(keys){Guarded<XINPUT_KEYSTROKE> key;result=keys(index,0,&key.value);check((result==ERROR_SUCCESS||result==ERROR_EMPTY||result==ERROR_DEVICE_NOT_CONNECTED)&&guarded(key),"keystroke query",result);}
        result=state(index,&value.value);if(result==ERROR_SUCCESS)++connected;
        check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED)&&guarded(value),"controller state query",result);
        if(result==ERROR_SUCCESS)check(!(value.value.Gamepad.wButtons&0x0400),"standard state masks guide button");
        result=caps(index,XINPUT_FLAG_GAMEPAD,&capabilities.value);
        check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED)&&guarded(capabilities),"controller capabilities query",result);
        if(extended){Guarded<XINPUT_STATE> ex;result=extended(index,&ex.value);check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED)&&guarded(ex),"extended state query",result);}
        if(extended_caps){Guarded<CapsEx> ex;result=extended_caps(1,index,0,&ex.value);check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED)&&guarded(ex),"extended capabilities query",result);}
        if(battery){Guarded<XINPUT_BATTERY_INFORMATION> info;result=battery(index,BATTERY_DEVTYPE_GAMEPAD,&info.value);check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED||result==ERROR_NOT_SUPPORTED)&&guarded(info),"battery query",result);}
        if(audio){
            UINT render_count=0,capture_count=0;result=audio(index,nullptr,&render_count,nullptr,&capture_count);
            check(result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED||result==ERROR_NOT_SUPPORTED,"audio identifier sizing query (identities not logged)",result);
            result=audio(index,nullptr,&render_count,nullptr,nullptr);
            check(result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED||result==ERROR_NOT_SUPPORTED,"audio render-only sizing query",result);
            result=audio(index,nullptr,nullptr,nullptr,&capture_count);
            check(result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED||result==ERROR_NOT_SUPPORTED,"audio capture-only sizing query",result);
        }
        if(sound){Guarded<GUID> render,capture;result=sound(index,&render.value,&capture.value);check((result==ERROR_SUCCESS||result==ERROR_DEVICE_NOT_CONNECTED||result==ERROR_NOT_SUPPORTED)&&guarded(render)&&guarded(capture),"DirectSound identifier query (identities not logged)",result);}
    }
    if(wine){
        HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
        auto load_backend=(HMODULE(WINAPI*)())GetProcAddress(shim,"WinedowsLoadSystemXInput");
        HMODULE backend=load_backend?load_backend():nullptr;
        check(backend&&backend!=module&&provider(backend,L"xinput1_4.dll",false),"trusted system hardware backend is separate from Wine frontend");
        if(backend){
            auto host_state=(State)GetProcAddress(backend,"XInputGetState");unsigned host_connected=0;
            for(DWORD index=0;host_state&&index<XUSER_MAX_COUNT;++index)if(host_state(index,&value.value)==ERROR_SUCCESS)++host_connected;
            printf("BACKEND connected=%u frontend_connected=%u\n",host_connected,connected);
            check(host_state&&host_connected==connected,"frontend and hardware backend controller availability");
            FreeLibrary(backend);
        }
        if(version91)check(provider(GetModuleHandleW(L"xinput1_4.dll"),L"xinput1_4.dll",true),"Wine 9.1.0 delegates to selected Wine 1.4");
    }
    printf("RESULT connected=%u failures=%d\n",connected,failures);
    puts("UNVERIFIED: physical button/axis transitions, hotplug, vibration and controller audio; no input injected or vibration requested.");
    FreeLibrary(module);return failures?1:0;
}
