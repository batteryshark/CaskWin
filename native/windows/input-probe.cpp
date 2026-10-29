// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise real DirectInput providers without synthesizing user input or
// recording any key states, mouse coordinates, or buffered event contents.
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <string>
#include <vector>
#ifdef WINEDOWS_STATIC_INPUT_IMPORTS
// dinput.h hides the legacy factory declaration at DIRECTINPUT_VERSION 0x0800.
extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
#endif

static int failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK) {
    printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",name,(unsigned long)hr);
    if(!ok)++failures;
}
static std::wstring module_path(HMODULE module) {
    std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());
    return n&&n<path.size()?std::wstring(path.data(),n):std::wstring();
}
static bool selected_provider(HMODULE module,const wchar_t *name,bool wine) {
    std::wstring path=module_path(module),expected;
    if(wine){
#ifdef _WIN64
        auto shim=GetModuleHandleW(L"winedows64.dll");const wchar_t *arch=L"x64";
#else
        auto shim=GetModuleHandleW(L"winedows32.dll");const wchar_t *arch=L"x86";
#endif
        if(!shim)return false;
        expected=module_path(shim);expected.resize(expected.find_last_of(L"\\/")+1);
        expected+=L"builtin\\"+std::wstring(arch)+L"\\"+name;
    }else{
        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);expected=std::wstring(system)+L"\\"+name;
    }
    wprintf(L"Provider %ls: %ls\n",name,path.c_str());
    return !_wcsicmp(path.c_str(),expected.c_str());
}
#ifdef WINEDOWS_STATIC_INPUT_IMPORTS
static bool imported_from(const char *dll,const char *symbol,HMODULE provider) {
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;imports++)if(!_stricmp((char*)base+imports->Name,dll)){
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk);
        auto bound=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;names++,bound++){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto name=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)name->Name,symbol))return (FARPROC)bound->u1.Function==GetProcAddress(provider,symbol);
        }
    }
    return false;
}
#endif
struct Enumeration {unsigned total=0,controllers=0;bool keyboard=false,mouse=false;};
static BOOL CALLBACK enum_devices(const DIDEVICEINSTANCEW *instance,void *context) {
    auto &result=*(Enumeration*)context;++result.total;
    if(IsEqualGUID(instance->guidInstance,GUID_SysKeyboard))result.keyboard=true;
    else if(IsEqualGUID(instance->guidInstance,GUID_SysMouse))result.mouse=true;
    else ++result.controllers;
    return DIENUM_CONTINUE;
}
struct Objects {unsigned keys=0,buttons=0,axes=0;};
static BOOL CALLBACK enum_objects(const DIDEVICEOBJECTINSTANCEW *object,void *context) {
    auto &result=*(Objects*)context;
    if(IsEqualGUID(object->guidType,GUID_Key))++result.keys;
    if(object->dwType&DIDFT_BUTTON)++result.buttons;
    if(object->dwType&DIDFT_AXIS)++result.axes;
    return DIENUM_CONTINUE;
}
static void pump() {MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
template<class Device> static void exercise(Device *device,HWND window,bool keyboard) {
    printf("Device fixture: %s\n",keyboard?"system keyboard":"system mouse");
    DIDEVICEINSTANCEW info{};info.dwSize=sizeof(info);
    HRESULT hr=device->GetDeviceInfo(&info);
    check(SUCCEEDED(hr)&&IsEqualGUID(info.guidInstance,keyboard?GUID_SysKeyboard:GUID_SysMouse),"device identity",hr);
    DIDEVCAPS caps{};caps.dwSize=sizeof(caps);hr=device->GetCapabilities(&caps);
    check(SUCCEEDED(hr)&&(keyboard?caps.dwButtons>=100:caps.dwAxes>=2),"device capabilities",hr);
    Objects objects;hr=device->EnumObjects(enum_objects,&objects,DIDFT_ALL);
    check(SUCCEEDED(hr)&&(keyboard?objects.keys>=100:objects.axes>=2),"device objects enumerate",hr);
    hr=device->SetDataFormat(keyboard?&c_dfDIKeyboard:&c_dfDIMouse2);
    check(SUCCEEDED(hr),"standard input data format",hr);
    hr=device->SetCooperativeLevel(window,DISCL_BACKGROUND|DISCL_NONEXCLUSIVE);
    check(SUCCEEDED(hr),"background nonexclusive cooperative level",hr);
    DIPROPDWORD buffer{};buffer.diph={sizeof(buffer),sizeof(buffer.diph),0,DIPH_DEVICE};buffer.dwData=32;
    hr=device->SetProperty(DIPROP_BUFFERSIZE,&buffer.diph);check(SUCCEEDED(hr),"input event buffer configuration",hr);
    buffer.dwData=0;hr=device->GetProperty(DIPROP_BUFFERSIZE,&buffer.diph);
    check(SUCCEEDED(hr)&&buffer.dwData==32,"input event buffer size readback",hr);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(event!=nullptr,"input event notification handle",HRESULT_FROM_WIN32(GetLastError()));
    if(event){hr=device->SetEventNotification(event);check(SUCCEEDED(hr),"input event notification registration",hr);}
    BYTE state[sizeof(DIMOUSESTATE2)>256?sizeof(DIMOUSESTATE2):256]{};
    DWORD size=keyboard?256:sizeof(DIMOUSESTATE2);
    hr=device->GetDeviceState(size,state);check(hr==DIERR_NOTACQUIRED,"state is unavailable before acquisition",hr);
    hr=device->Acquire();check(SUCCEEDED(hr),"device acquisition",hr);
    if(SUCCEEDED(hr)){
        pump();hr=device->Poll();check(SUCCEEDED(hr)||hr==DI_NOEFFECT,"device polling",hr);
        hr=device->GetDeviceState(size,state);check(SUCCEEDED(hr),"acquired device state read",hr);
        // Do not print state bytes: they could contain the user's live input.
        DIDEVICEOBJECTDATA events[32]{};DWORD count=32;
        hr=device->GetDeviceData(sizeof(events[0]),events,&count,DIGDD_PEEK);
        check((SUCCEEDED(hr)||hr==DI_BUFFEROVERFLOW)&&count<=32,"buffered input API read",hr);
        hr=device->Unacquire();check(SUCCEEDED(hr),"device unacquisition",hr);
        hr=device->GetDeviceState(size,state);check(hr==DIERR_NOTACQUIRED,"state unavailable after unacquisition",hr);
    }
    if(event){device->SetEventNotification(nullptr);CloseHandle(event);}
}
template<class Input> static void enumerate(Input *input) {
    Enumeration result;HRESULT hr=input->EnumDevices(0,enum_devices,&result,DIEDFL_ATTACHEDONLY);
    check(SUCCEEDED(hr)&&result.keyboard&&result.mouse,"attached system keyboard and mouse enumeration",hr);
    printf("Observed %u input device(s), %u other/controller device(s).\n",result.total,result.controllers);
    puts("NOT TESTED: physical controller events, force feedback, or fresh key/mouse delivery; no user input was synthesized.");
}
static bool child(const wchar_t *program,bool native7,bool native8) {
    fflush(stdout);
    std::wstring command=L"\""+std::wstring(program)+L"\"";
    if(native7)command+=L" --native-dinput";
    if(native8)command+=L" --native-dinput8";
    wchar_t environment[]=L"INPUT_CHILD_FIXTURE=1\0\0";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(program,command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,nullptr,&startup,&process))return false;
    DWORD code=1,end=GetTickCount()+30000;
    while((LONG)(end-GetTickCount())>0){
        if(MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,100,QS_ALLINPUT)==WAIT_OBJECT_0){GetExitCodeProcess(process.hProcess,&code);break;}
        pump();
    }
    if(code)TerminateProcess(process.hProcess,99);
    CloseHandle(process.hProcess);CloseHandle(process.hThread);return code==0;
}
int wmain(int argc,wchar_t **argv) {
    bool native7=false,native8=false;const wchar_t *child_program=nullptr;
    for(int i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--native"))native7=native8=true;
        else if(!wcscmp(argv[i],L"--native-dinput"))native7=true;
        else if(!wcscmp(argv[i],L"--native-dinput8"))native8=true;
        else if(!wcscmp(argv[i],L"--child-program")&&i+1<argc)child_program=argv[++i];
    }
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    check(SUCCEEDED(co),"COM initialized for input fixture",co);
    HMODULE old=LoadLibraryW(L"dinput.dll"),modern=LoadLibraryW(L"dinput8.dll");
    check(old&&modern,"load input providers",HRESULT_FROM_WIN32(GetLastError()));
    if(!old||!modern)return 1;
    check(selected_provider(old,L"dinput.dll",!native7),"selected DirectInput provider");
    check(selected_provider(modern,L"dinput8.dll",!native8),"selected DirectInput8 provider");
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows input probe",WS_OVERLAPPEDWINDOW,0,0,160,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"input cooperative window",HRESULT_FROM_WIN32(GetLastError()));
    wchar_t desktop_handle[32]{};
    if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",desktop_handle,32)>0&&desktop_handle[0]){
        ShowWindow(window,SW_SHOWNOACTIVATE);check(IsChild(GetDesktopWindow(),window)!=FALSE,"input window is contained in virtual desktop");
    }
    using Create7=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
    using Create8=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
#ifdef WINEDOWS_STATIC_INPUT_IMPORTS
    auto create7=(Create7)&DirectInputCreateEx;auto create8=(Create8)&DirectInput8Create;
    check(imported_from("dinput.dll","DirectInputCreateEx",old),"static DirectInput import uses selected provider");
    check(imported_from("dinput8.dll","DirectInput8Create",modern),"static DirectInput8 import uses selected provider");
#else
    auto create7=(Create7)GetProcAddress(old,"DirectInputCreateEx");auto create8=(Create8)GetProcAddress(modern,"DirectInput8Create");
#endif
    check(create7&&create8,"input factory exports");
    if(create7){
        IDirectInput7W *input=nullptr;HRESULT hr=create7(GetModuleHandleW(nullptr),0x0700,IID_IDirectInput7W,(void**)&input,nullptr);
        check(SUCCEEDED(hr)&&input,"DirectInput7 creation",hr);
        if(input){
            enumerate(input);
            for(bool keyboard:{true,false}){
                IDirectInputDevice7W *device=nullptr;hr=input->CreateDeviceEx(keyboard?GUID_SysKeyboard:GUID_SysMouse,IID_IDirectInputDevice7W,(void**)&device,nullptr);
                check(SUCCEEDED(hr)&&device,"DirectInput7 system device creation",hr);
                if(device){exercise(device,window,keyboard);device->Release();}
            }
            input->Release();
        }
    }
    if(create8){
        IDirectInput8W *input=nullptr;HRESULT hr=create8(GetModuleHandleW(nullptr),0x0800,IID_IDirectInput8W,(void**)&input,nullptr);
        check(SUCCEEDED(hr)&&input,"DirectInput8 creation",hr);
        if(input){
            enumerate(input);
            for(bool keyboard:{true,false}){
                IDirectInputDevice8W *device=nullptr;hr=input->CreateDevice(keyboard?GUID_SysKeyboard:GUID_SysMouse,&device,nullptr);
                check(SUCCEEDED(hr)&&device,"DirectInput8 system device creation",hr);
                if(device){exercise(device,window,keyboard);device->Release();}
            }
            input->Release();
        }
    }
    if(window)DestroyWindow(window);
    FreeLibrary(old);FreeLibrary(modern);if(SUCCEEDED(co))CoUninitialize();
    if(child_program)check(child(child_program,native7,native8),"input provider choice survives cross-bitness child/custom environment");
    printf("Input %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);
    return failures?1:0;
}
