// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise actual DirectSound playback through the selected Wine/native DLL.
// Playback data is zero and volume is muted; this fixture never opens capture.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTSOUND_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK){
    printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",name,(unsigned long)hr);
    if(!ok)++failures;
}
static std::wstring path_of(HMODULE module){
    std::vector<wchar_t> path(32768);DWORD size=GetModuleFileNameW(module,path.data(),(DWORD)path.size());
    return size&&size<path.size()?std::wstring(path.data(),size):std::wstring();
}
static std::wstring absolute_path(const wchar_t *path){
    std::vector<wchar_t> out(32768);DWORD size=GetFullPathNameW(path,(DWORD)out.size(),out.data(),nullptr);
    return size&&size<out.size()?std::wstring(out.data(),size):std::wstring();
}
static bool owned_by(void *object,HMODULE provider){
    if(!object)return false;
    HMODULE module=nullptr;void **table=*(void***)object;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)table[0],&module)&&module==provider;
}
#ifdef WINEDOWS_STATIC_AUDIO_IMPORTS
static bool imported_from(const char *symbol,HMODULE provider){
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;
    auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;imports++)if(!_stricmp((char*)base+imports->Name,"dsound.dll")){
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk),bound=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;names++,bound++){
            // Microsoft's dsound import library binds these exports by their
            // public ordinals; MinGW may use names. Verify either form.
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)){
                auto imported=GetProcAddress(provider,MAKEINTRESOURCEA(IMAGE_ORDINAL(names->u1.Ordinal)));
                if(imported==GetProcAddress(provider,symbol))return (FARPROC)bound->u1.Function==imported;
                continue;
            }
            auto item=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)item->Name,symbol))return (FARPROC)bound->u1.Function==GetProcAddress(provider,symbol);
        }
    }
    return false;
}
#endif
struct Endpoints {unsigned total=0,explicit_devices=0;};
static BOOL CALLBACK enumerate(GUID *guid,LPCWSTR,LPCWSTR,void *context){
    auto endpoints=(Endpoints*)context;++endpoints->total;if(guid)++endpoints->explicit_devices;return TRUE;
}

int wmain(int argc,wchar_t **argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    bool native=false;std::wstring expected;
    for(int i=1;i<argc;i++){
        if(!wcscmp(argv[i],L"--native"))native=true;
        else if(!wcscmp(argv[i],L"--expected-dll")&&i+1<argc)expected=absolute_path(argv[++i]);
        else {fwprintf(stderr,L"Unknown audio fixture argument: %ls\n",argv[i]);return 2;}
    }
    if(expected.empty()&&native){wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);expected=std::wstring(system)+L"\\dsound.dll";}
    if(expected.empty()){fputs("Specify --expected-dll for Wine or --native for Windows.\n",stderr);return 2;}
    HMODULE module=LoadLibraryW(L"dsound.dll");
    check(module!=nullptr,"load DirectSound provider",module?S_OK:HRESULT_FROM_WIN32(GetLastError()));if(!module)return 1;
    std::wstring actual=path_of(module);wprintf(L"DirectSound module: %ls\n",actual.c_str());
    check(!_wcsicmp(actual.c_str(),expected.c_str()),"selected DirectSound provider has the expected absolute path");
    if(failures){FreeLibrary(module);return 1;}

    IMMDeviceEnumerator *uninitialized=nullptr;
    HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),(void**)&uninitialized);
    check(hr==CO_E_NOTINITIALIZED&&!uninitialized,"audio COM preserves the initialization requirement",hr);
    if(uninitialized)uninitialized->Release();

    // A failed COM registration/activation is a backend failure. A genuinely
    // absent render endpoint is distinct: exit 77 reports incomplete coverage.
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(initialized),"initialize COM",initialized);
    if(FAILED(initialized)){FreeLibrary(module);return 1;}
    IMMDeviceEnumerator *endpoint_api=nullptr;
    hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),(void**)&endpoint_api);
    check(SUCCEEDED(hr),"activate host WASAPI endpoint enumerator",hr);
    if(FAILED(hr)){CoUninitialize();FreeLibrary(module);return 1;}
    HMODULE backend=GetModuleHandleW(L"MMDevApi.dll");wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    std::wstring expected_backend=std::wstring(system)+L"\\MMDevApi.dll";
    wprintf(L"WASAPI backend module: %ls\n",path_of(backend).c_str());
    check(backend&&owned_by(endpoint_api,backend)&&!_wcsicmp(path_of(backend).c_str(),expected_backend.c_str()),"endpoint enumerator uses the native system MMDevApi implementation");
    IMMDevice *endpoint=nullptr;hr=endpoint_api->GetDefaultAudioEndpoint(eRender,eMultimedia,&endpoint);
    if(hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)){
        puts("SKIP no default render endpoint; playback coverage is incomplete (exit 77)");
        endpoint_api->Release();CoUninitialize();FreeLibrary(module);return 77;
    }
    check(SUCCEEDED(hr),"resolve default host render endpoint",hr);
    if(endpoint){DWORD state=0;hr=endpoint->GetState(&state);check(SUCCEEDED(hr)&&(state&DEVICE_STATE_ACTIVE),"host render endpoint is active",hr);endpoint->Release();}
    endpoint_api->Release();
    if(failures){CoUninitialize();FreeLibrary(module);return 1;}

    const IID unsupported={0x30c7b9e3,0xfca6,0x4eaf,{0x93,0xa3,0x1b,0x0e,0x36,0x09,0xbb,0x84}};
    const IID endpoint_iid=__uuidof(IMMDeviceEnumerator);
    MULTI_QI endpoint_queries[]={{&endpoint_iid,nullptr,E_FAIL},{&unsupported,nullptr,E_FAIL}};
    hr=CoCreateInstanceEx(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,nullptr,2,endpoint_queries);
    check(hr==CO_S_NOTALLINTERFACES&&endpoint_queries[0].hr==S_OK&&endpoint_queries[0].pItf&&endpoint_queries[1].hr==E_NOINTERFACE&&!endpoint_queries[1].pItf,"extended endpoint COM activation preserves partial interface results",hr);
    for(auto &query:endpoint_queries)if(query.pItf)query.pItf->Release();
    IUnknown *unknown=nullptr;hr=CoCreateInstance(unsupported,nullptr,CLSCTX_INPROC_SERVER,IID_IUnknown,(void**)&unknown);
    check(hr==REGDB_E_CLASSNOTREG&&!unknown,"unrelated COM class remains unregistered in the prefix",hr);
    if(unknown)unknown->Release();

    using Enumerate=HRESULT(WINAPI*)(LPDSENUMCALLBACKW,void*);
    using Create8=HRESULT(WINAPI*)(const GUID*,IDirectSound8**,IUnknown*);
    using Create=HRESULT(WINAPI*)(const GUID*,IDirectSound**,IUnknown*);
#ifdef WINEDOWS_STATIC_AUDIO_IMPORTS
    Enumerate enum_devices=&DirectSoundEnumerateW;Create8 create8=&DirectSoundCreate8;Create create=&DirectSoundCreate;
    check(imported_from("DirectSoundCreate8",module)&&imported_from("DirectSoundEnumerateW",module)&&imported_from("DirectSoundCreate",module),"startup DirectSound imports bind to selected provider");
#else
    auto enum_devices=(Enumerate)GetProcAddress(module,"DirectSoundEnumerateW");
    auto create8=(Create8)GetProcAddress(module,"DirectSoundCreate8");
    auto create=(Create)GetProcAddress(module,"DirectSoundCreate");
#endif
    check(enum_devices&&create8&&create,"DirectSound playback exports are available");
    if(!enum_devices||!create8||!create){CoUninitialize();FreeLibrary(module);return 1;}
    Endpoints devices;hr=enum_devices(enumerate,&devices);
    printf("DirectSound playback devices: %u entries, %u explicit endpoints\n",devices.total,devices.explicit_devices);
    check(SUCCEEDED(hr)&&devices.total&&devices.explicit_devices,"DirectSound enumerates the host playback endpoints",hr);
    IDirectSound *com_legacy=nullptr;hr=CoCreateInstance(CLSID_DirectSound,nullptr,CLSCTX_INPROC_SERVER,IID_IDirectSound,(void**)&com_legacy);
    check(SUCCEEDED(hr)&&com_legacy,"COM creates legacy DirectSound",hr);
    if(com_legacy){check(owned_by(com_legacy,module),"COM legacy DirectSound uses the selected provider");hr=com_legacy->Initialize(nullptr);check(SUCCEEDED(hr),"COM legacy DirectSound initializes the playback endpoint",hr);com_legacy->Release();}
    IDirectSound8 *com_sound=nullptr;hr=CoCreateInstance(CLSID_DirectSound8,nullptr,CLSCTX_INPROC_SERVER,IID_IDirectSound8,(void**)&com_sound);
    check(SUCCEEDED(hr)&&com_sound,"COM creates DirectSound8",hr);
    if(com_sound){check(owned_by(com_sound,module),"COM DirectSound8 uses the selected provider");hr=com_sound->Initialize(nullptr);check(SUCCEEDED(hr),"COM DirectSound8 initializes the playback endpoint",hr);com_sound->Release();}
    MULTI_QI sound_queries[]={{&IID_IDirectSound8,nullptr,E_FAIL},{&IID_IDirectSound,nullptr,E_FAIL},{&unsupported,nullptr,E_FAIL}};
    hr=CoCreateInstanceEx(CLSID_DirectSound8,nullptr,CLSCTX_INPROC_SERVER,nullptr,3,sound_queries);
    check(hr==CO_S_NOTALLINTERFACES&&sound_queries[0].hr==S_OK&&sound_queries[0].pItf&&sound_queries[1].hr==S_OK&&sound_queries[1].pItf&&sound_queries[2].hr==E_NOINTERFACE&&!sound_queries[2].pItf,"extended DirectSound COM activation preserves partial interface results",hr);
    for(auto &query:sound_queries)if(query.pItf){check(owned_by(query.pItf,module),"extended COM interface uses selected DirectSound provider");query.pItf->Release();}
    IDirectSound *legacy=nullptr;hr=create(nullptr,&legacy,nullptr);
    check(SUCCEEDED(hr)&&legacy,"legacy DirectSound creation",hr);
    if(legacy){check(owned_by(legacy,module),"legacy DirectSound interface belongs to selected provider");legacy->Release();}
    IDirectSound8 *sound=nullptr;hr=create8(nullptr,&sound,nullptr);
    check(SUCCEEDED(hr)&&sound,"DirectSound8 creation",hr);
    if(!sound){CoUninitialize();FreeLibrary(module);return 1;}
    check(owned_by(sound,module),"DirectSound8 interface belongs to selected provider");
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows silent audio fixture",WS_OVERLAPPEDWINDOW,0,0,160,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    hr=sound->SetCooperativeLevel(window,DSSCL_NORMAL);check(SUCCEEDED(hr),"normal cooperative playback level",hr);
    DSCAPS caps{};caps.dwSize=sizeof(caps);hr=sound->GetCaps(&caps);check(SUCCEEDED(hr),"query playback capabilities",hr);
    WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=44100;
    format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=format.nSamplesPerSec*format.nBlockAlign;
    DSBUFFERDESC desc{};desc.dwSize=sizeof(desc);desc.dwBufferBytes=format.nAvgBytesPerSec;desc.lpwfxFormat=&format;
    desc.dwFlags=DSBCAPS_LOCSOFTWARE|DSBCAPS_CTRLVOLUME|DSBCAPS_CTRLPAN|DSBCAPS_CTRLFREQUENCY|DSBCAPS_CTRLPOSITIONNOTIFY|DSBCAPS_GETCURRENTPOSITION2|DSBCAPS_GLOBALFOCUS;
    IDirectSoundBuffer *buffer=nullptr;hr=sound->CreateSoundBuffer(&desc,&buffer,nullptr);check(SUCCEEDED(hr)&&buffer,"create PCM playback buffer",hr);
    if(buffer){
        check(owned_by(buffer,module),"buffer implementation belongs to selected provider");
        DSBCAPS bcaps{};bcaps.dwSize=sizeof(bcaps);hr=buffer->GetCaps(&bcaps);
        check(SUCCEEDED(hr)&&bcaps.dwBufferBytes==desc.dwBufferBytes,"buffer size is preserved",hr);
        WAVEFORMATEX queried{};DWORD written=0;hr=buffer->GetFormat(&queried,sizeof(queried),&written);
        check(SUCCEEDED(hr)&&queried.wFormatTag==WAVE_FORMAT_PCM&&queried.nChannels==2&&queried.nSamplesPerSec==44100&&queried.wBitsPerSample==16,"PCM format round trip",hr);
        void *first=nullptr,*second=nullptr;DWORD first_bytes=0,second_bytes=0;bool whole_silent=false,pattern_cleared=true;
        hr=buffer->Lock(0,0,&first,&first_bytes,&second,&second_bytes,DSBLOCK_ENTIREBUFFER);
        check(SUCCEEDED(hr)&&first_bytes+second_bytes==desc.dwBufferBytes,"lock whole playback buffer",hr);
        if(SUCCEEDED(hr)){memset(first,0,first_bytes);if(second)memset(second,0,second_bytes);hr=buffer->Unlock(first,first_bytes,second,second_bytes);whole_silent=SUCCEEDED(hr)&&first_bytes+second_bytes==desc.dwBufferBytes;check(SUCCEEDED(hr),"unlock silent PCM data",hr);}
        hr=buffer->Lock(desc.dwBufferBytes-16,32,&first,&first_bytes,&second,&second_bytes,0);
        check(SUCCEEDED(hr)&&first_bytes==16&&second_bytes==16,"circular buffer lock wraps at boundary",hr);
        if(SUCCEEDED(hr)){pattern_cleared=false;memset(first,0x5a,first_bytes);if(second)memset(second,0x5a,second_bytes);buffer->Unlock(first,first_bytes,second,second_bytes);}
        hr=buffer->Lock(desc.dwBufferBytes-16,32,&first,&first_bytes,&second,&second_bytes,0);
        bool persisted=SUCCEEDED(hr)&&first_bytes+second_bytes==32;
        if(SUCCEEDED(hr)){
            for(DWORD i=0;i<first_bytes;i++)if(((BYTE*)first)[i]!=0x5a)persisted=false;
            for(DWORD i=0;i<second_bytes;i++)if(((BYTE*)second)[i]!=0x5a)persisted=false;
            memset(first,0,first_bytes);if(second)memset(second,0,second_bytes);pattern_cleared=SUCCEEDED(buffer->Unlock(first,first_bytes,second,second_bytes))&&first_bytes+second_bytes==32;
        }
        check(persisted,"locked PCM bytes persist across unlock and wrap",hr);
        LONG volume=0,pan=0;DWORD frequency=0;
        hr=buffer->SetVolume(DSBVOLUME_MIN);if(SUCCEEDED(hr))hr=buffer->GetVolume(&volume);
        bool muted=SUCCEEDED(hr)&&volume==DSBVOLUME_MIN;check(muted,"playback is muted",hr);
        hr=buffer->SetPan(-2500);if(SUCCEEDED(hr))hr=buffer->GetPan(&pan);check(SUCCEEDED(hr)&&pan==-2500,"pan control round trip",hr);
        buffer->SetPan(0);
        hr=buffer->SetFrequency(22050);if(SUCCEEDED(hr))hr=buffer->GetFrequency(&frequency);check(SUCCEEDED(hr)&&frequency==22050,"playback frequency control",hr);
        buffer->SetFrequency(format.nSamplesPerSec);
        IDirectSoundBuffer *duplicate=nullptr;hr=sound->DuplicateSoundBuffer(buffer,&duplicate);check(SUCCEEDED(hr)&&duplicate,"duplicate secondary buffer",hr);if(duplicate)duplicate->Release();
        IDirectSoundNotify *notify=nullptr;hr=buffer->QueryInterface(IID_IDirectSoundNotify,(void**)&notify);check(SUCCEEDED(hr)&&notify,"buffer notification interface",hr);
        HANDLE position_event=CreateEventW(nullptr,FALSE,FALSE,nullptr),stop_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(position_event&&stop_event,"playback notification events",HRESULT_FROM_WIN32(GetLastError()));
        if(notify&&position_event&&stop_event){DSBPOSITIONNOTIFY positions[]={{desc.dwBufferBytes/4,position_event},{(DWORD)DSBPN_OFFSETSTOP,stop_event}};hr=notify->SetNotificationPositions(2,positions);check(SUCCEEDED(hr),"set playback and stop notifications",hr);}
        buffer->SetCurrentPosition(0);
        hr=whole_silent&&pattern_cleared&&muted?buffer->Play(0,0,DSBPLAY_LOOPING):E_FAIL;
        check(SUCCEEDED(hr),"start muted looping playback",hr);
        if(SUCCEEDED(hr)){
            DWORD status=0;hr=buffer->GetStatus(&status);check(SUCCEEDED(hr)&&(status&DSBSTATUS_PLAYING)&&(status&DSBSTATUS_LOOPING),"playback status reports running loop",hr);
            DWORD initial_play=0,initial_write=0,play=0,write=0;buffer->GetCurrentPosition(&initial_play,&initial_write);bool advanced=false,positions_valid=true;
            for(unsigned i=0;i<30;i++){
                Sleep(20);hr=buffer->GetCurrentPosition(&play,&write);
                if(FAILED(hr)||play>=desc.dwBufferBytes||write>=desc.dwBufferBytes||play%format.nBlockAlign||write%format.nBlockAlign)positions_valid=false;
                if(SUCCEEDED(hr)&&play!=initial_play)advanced=true;
            }
            printf("Playback cursors: start=%lu end=%lu write=%lu (buffer=%lu)\n",initial_play,play,write,desc.dwBufferBytes);
            check(advanced&&positions_valid,"endpoint-backed playback cursor advances and stays frame-aligned",hr);
            if(notify&&position_event)check(WaitForSingleObject(position_event,1200)==WAIT_OBJECT_0,"playback position notification arrives");
        }
        hr=buffer->Stop();check(SUCCEEDED(hr),"stop playback",hr);
        DWORD status=0;hr=buffer->GetStatus(&status);check(SUCCEEDED(hr)&&!(status&DSBSTATUS_PLAYING),"stopped status clears playing",hr);
        if(notify&&stop_event)check(WaitForSingleObject(stop_event,1200)==WAIT_OBJECT_0,"stop notification arrives");
        hr=buffer->SetCurrentPosition(desc.dwBufferBytes/2);DWORD seek=0;if(SUCCEEDED(hr))hr=buffer->GetCurrentPosition(&seek,nullptr);
        check(SUCCEEDED(hr)&&seek==desc.dwBufferBytes/2,"stopped buffer seek round trip",hr);
        if(notify)notify->Release();
        if(position_event)CloseHandle(position_event);
        if(stop_event)CloseHandle(stop_event);
        buffer->Release();
    }
    sound->Release();if(window)DestroyWindow(window);CoUninitialize();FreeLibrary(module);
    printf("%s DirectSound %zu-bit: %d failure(s); silent render only, no capture\n",native?"Native":"Wine",sizeof(void*)*8,failures);
    return failures?1:0;
}
