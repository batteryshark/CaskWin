// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual endpoint-backed XAudio2 streaming, with silent buffers and a muted master.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include "xaudio-probe-api.h"
#include <x3daudio.h>
#include <xapo.h>
#include <xapofx.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>

static int failures;
static void check(bool ok,const char *message,HRESULT hr=S_OK){printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",message,(unsigned long)hr);if(!ok)++failures;}
static std::wstring path_of(HMODULE module){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return n&&n<path.size()?std::wstring(path.data(),n):L"";}
static bool native_path(const std::wstring &path){wchar_t system[MAX_PATH];UINT n=GetSystemDirectoryW(system,MAX_PATH);return n&&path.size()>n&&!_wcsnicmp(path.c_str(),system,n)&&path[n]==L'\\';}
static bool owned_by(void *object,HMODULE provider){
    HMODULE module=nullptr;void **table=object?*(void***)object:nullptr;
    if(!table||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)table[0],&module))return false;
    if(module==provider)return true;
    // Windows 2.8 forwards engine/voice implementation to its system 2.9 DLL.
    // Wine providers still require an exact match, never a native substitute.
    if(native_path(path_of(provider))&&native_path(path_of(module))){wprintf(L"Native implementation: %ls\n",path_of(module).c_str());return true;}
    return false;
}
static bool expected_provider(HMODULE module,const wchar_t *expected){auto path=path_of(module);wprintf(L"Provider: %ls\n",path.c_str());return !wcscmp(expected,L"native")?native_path(path):!_wcsicmp(path.c_str(),expected);}

constexpr unsigned chunks=48,frames_per_chunk=2205;
struct VoiceCallbacks final:IXAudio2VoiceCallback {
    HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::atomic<unsigned> started{0},ended{0},streams{0},errors{0},bad_order{0},passes{0};
    ~VoiceCallbacks(){if(wake)CloseHandle(wake);}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override{++passes;}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override{}
    void STDMETHODCALLTYPE OnStreamEnd() override{++streams;SetEvent(wake);}
    void STDMETHODCALLTYPE OnBufferStart(void *context) override{unsigned n=started.fetch_add(1);if((uintptr_t)context!=n+1)++bad_order;}
    void STDMETHODCALLTYPE OnBufferEnd(void *context) override{unsigned n=ended.fetch_add(1);if((uintptr_t)context!=n+1)++bad_order;SetEvent(wake);}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override{}
    void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT) override{++errors;SetEvent(wake);}
};
struct EngineCallbacks final:IXAudio2EngineCallback {
    std::atomic<unsigned> passes{0},errors{0};
    void STDMETHODCALLTYPE OnProcessingPassStart() override{++passes;}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override{}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override{++errors;}
};
static void voice_state(IXAudio2SourceVoice *voice,XAUDIO2_VOICE_STATE &state){
#if XAUDIO2_VER <= 7
    voice->GetState(&state);
#else
    voice->GetState(&state,0);
#endif
}
static bool imported_from(const char *symbol,const char *dll,HMODULE provider){
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;imports++)if(!_stricmp((char*)base+imports->Name,dll)){
        if(!imports->OriginalFirstThunk)return false;
        auto names=(IMAGE_THUNK_DATA*)(base+imports->OriginalFirstThunk),bound=(IMAGE_THUNK_DATA*)(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;names++,bound++){
            FARPROC requested=nullptr;
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))requested=GetProcAddress(provider,MAKEINTRESOURCEA(IMAGE_ORDINAL(names->u1.Ordinal)));
            else{auto item=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);if(!strcmp((char*)item->Name,symbol))requested=GetProcAddress(provider,symbol);}
            if(requested==GetProcAddress(provider,symbol))return (FARPROC)bound->u1.Function==requested;
        }
    }return false;
}

static void stream(IXAudio2 *engine,HMODULE provider){
    EngineCallbacks engine_callbacks;VoiceCallbacks callbacks;
    check(callbacks.wake!=nullptr,"create streaming callback event");
    HRESULT hr=engine->RegisterForCallbacks(&engine_callbacks);check(SUCCEEDED(hr),"register engine callbacks",hr);
    IXAudio2MasteringVoice *master=nullptr;
#if XAUDIO2_VER <= 7
    UINT32 devices=0;hr=engine->GetDeviceCount(&devices);check(SUCCEEDED(hr)&&devices>0,"legacy endpoint enumeration",hr);
    hr=engine->CreateMasteringVoice(&master,2,48000,0,0,nullptr);
#else
    hr=engine->CreateMasteringVoice(&master,2,48000,0,nullptr,nullptr,AudioCategory_GameEffects);
#endif
    check(SUCCEEDED(hr)&&master,"create native-endpoint mastering voice",hr);
    if(!master){engine->UnregisterForCallbacks(&engine_callbacks);return;}
    hr=master->SetVolume(0.0f,XAUDIO2_COMMIT_NOW);float master_volume=1.0f;master->GetVolume(&master_volume);
    bool muted=SUCCEEDED(hr)&&master_volume==0.0f;check(muted,"master voice is muted before playback",hr);
    XAUDIO2_VOICE_DETAILS details{};master->GetVoiceDetails(&details);check(details.InputChannels==2&&details.InputSampleRate==48000,"48kHz stereo mastering format");
    IXAudio2SubmixVoice *mix=nullptr;hr=engine->CreateSubmixVoice(&mix,2,48000,0,0,nullptr,nullptr);check(SUCCEEDED(hr)&&mix,"create stereo submix",hr);
    if(!mix){master->DestroyVoice();engine->UnregisterForCallbacks(&engine_callbacks);return;}
    XAUDIO2_SEND_DESCRIPTOR destination{0,mix};XAUDIO2_VOICE_SENDS sends{1,&destination};
    WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=1;format.nSamplesPerSec=44100;format.wBitsPerSample=16;format.nBlockAlign=2;format.nAvgBytesPerSec=88200;
    IXAudio2SourceVoice *source=nullptr;hr=engine->CreateSourceVoice(&source,&format,0,2.0f,&callbacks,&sends,nullptr);check(SUCCEEDED(hr)&&source,"create 44.1kHz mono source feeding 48kHz stereo",hr);
    if(!source){mix->DestroyVoice();master->DestroyVoice();engine->UnregisterForCallbacks(&engine_callbacks);return;}
    check(owned_by(source,provider),"source voice vtable belongs to selected provider");
    float matrix[2]={0.5f,0.5f},round_trip[2]{};hr=source->SetOutputMatrix(mix,1,2,matrix,XAUDIO2_COMMIT_NOW);source->GetOutputMatrix(mix,1,2,round_trip);
    check(SUCCEEDED(hr)&&round_trip[0]==0.5f&&round_trip[1]==0.5f,"mono-to-stereo output matrix round trip",hr);
    hr=source->SetFrequencyRatio(0.75f,XAUDIO2_COMMIT_NOW);float ratio=0;
    // Native XAudio applies COMMIT_NOW at its next processing quantum.
    for(unsigned retry=0;retry<50;retry++){source->GetFrequencyRatio(&ratio);if(std::fabs(ratio-0.75f)<0.0001f)break;Sleep(10);}
    printf("Frequency ratio: %.9g\n",ratio);check(SUCCEEDED(hr)&&std::fabs(ratio-0.75f)<0.0001f,"frequency ratio control reaches the requested ratio",hr);
    hr=source->SetFrequencyRatio(1.0f,XAUDIO2_COMMIT_NOW);check(SUCCEEDED(hr),"restore normal sample rate",hr);
    std::vector<short> silence(chunks*frames_per_chunk,0);unsigned submitted=0;
    auto submit=[&](){XAUDIO2_BUFFER buffer{};buffer.AudioBytes=frames_per_chunk*sizeof(short);buffer.pAudioData=(BYTE*)(silence.data()+submitted*frames_per_chunk);buffer.pContext=(void*)(uintptr_t)(submitted+1);if(submitted+1==chunks)buffer.Flags=XAUDIO2_END_OF_STREAM;HRESULT result=source->SubmitSourceBuffer(&buffer,nullptr);if(SUCCEEDED(result))++submitted;return result;};
    bool submitted_ok=true;for(unsigned i=0;i<4;i++)if(FAILED(submit()))submitted_ok=false;
    check(submitted_ok,"queue initial silent streaming buffers");
    hr=engine->StartEngine();check(SUCCEEDED(hr),"start XAudio engine",hr);
    if(muted&&submitted_ok){hr=source->Start(0,XAUDIO2_COMMIT_NOW);check(SUCCEEDED(hr),"start queued silent source",hr);}
    UINT64 maximum_samples=0,last_samples=0;unsigned underruns=0;bool monotonic=true,paused=false;
    ULONGLONG deadline=GetTickCount64()+15000;
    while(muted&&submitted_ok&&callbacks.ended<chunks&&GetTickCount64()<deadline&&!callbacks.errors&&!engine_callbacks.errors){
        XAUDIO2_VOICE_STATE state{};voice_state(source,state);maximum_samples=std::max(maximum_samples,state.SamplesPlayed);
        if(state.SamplesPlayed<last_samples&&callbacks.streams==0)monotonic=false;
        last_samples=state.SamplesPlayed;
        if(submitted<chunks&&state.BuffersQueued==0)++underruns;
        while(submitted<chunks&&state.BuffersQueued<4){if(FAILED(submit())){submitted_ok=false;break;}++state.BuffersQueued;}
        if(!paused&&callbacks.ended>=12){
            hr=source->Stop(0,XAUDIO2_COMMIT_NOW);check(SUCCEEDED(hr),"pause source without discarding queued buffers",hr);Sleep(40);
            XAUDIO2_VOICE_STATE before{},after{};voice_state(source,before);Sleep(100);voice_state(source,after);
            check(before.SamplesPlayed==after.SamplesPlayed&&before.BuffersQueued==after.BuffersQueued,"paused sample and queue positions remain fixed");
            hr=source->Start(0,XAUDIO2_COMMIT_NOW);check(SUCCEEDED(hr),"resume streaming source",hr);paused=true;
        }
        WaitForSingleObject(callbacks.wake,5);
    }
    Sleep(30);XAUDIO2_VOICE_STATE final{};voice_state(source,final);maximum_samples=std::max(maximum_samples,final.SamplesPlayed);
    printf("Streaming: submitted=%u started=%u ended=%u stream_end=%u max_samples=%llu final_samples=%llu underruns=%u engine_passes=%u voice_passes=%u\n",submitted,callbacks.started.load(),callbacks.ended.load(),callbacks.streams.load(),(unsigned long long)maximum_samples,(unsigned long long)final.SamplesPlayed,underruns,engine_callbacks.passes.load(),callbacks.passes.load());
    check(submitted_ok&&submitted==chunks&&callbacks.started==chunks&&callbacks.ended==chunks,"every streamed buffer receives start/end callbacks");
    check(callbacks.streams==1&&final.BuffersQueued==0,"end-of-stream callback drains the queue");
    check(callbacks.bad_order==0,"streaming callback contexts preserve submission order");
    check(monotonic&&maximum_samples>=frames_per_chunk*(chunks-4),"real source sample position progresses through resampling");
    check(underruns==0,"stream refill keeps the source queue nonempty until completion");
    check(callbacks.errors==0&&engine_callbacks.errors==0&&callbacks.passes>0&&engine_callbacks.passes>0,"audio processing runs without voice or engine errors");
    source->Stop(0,XAUDIO2_COMMIT_NOW);source->FlushSourceBuffers();source->DestroyVoice();mix->DestroyVoice();master->DestroyVoice();engine->UnregisterForCallbacks(&engine_callbacks);engine->StopEngine();
}

// XAUDIO2FX_VOLUMEMETER_LEVELS is packed to one byte by the DirectX ABI.
#pragma pack(push,1)
struct MeterLevels {float *peak,*rms;UINT32 channels;};
#pragma pack(pop)
static void volume_meter(HMODULE provider,bool use_com){
    IUnknown *object=nullptr;HRESULT hr=E_FAIL;
#if XAUDIO2_VER <= 7
    if(use_com)hr=CoCreateInstance(CLSID_AudioVolumeMeter,nullptr,CLSCTX_INPROC_SERVER,IID_IUnknown,(void**)&object);
    else{
    using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);auto factory=(Factory)GetProcAddress(provider,"DllGetClassObject");IClassFactory *creator=nullptr;
    hr=factory?factory(CLSID_AudioVolumeMeter,IID_IClassFactory,(void**)&creator):E_NOINTERFACE;
    if(creator){hr=creator->CreateInstance(nullptr,IID_IUnknown,(void**)&object);creator->Release();}
    }
    const IID &effect_iid=IID_IXAPO27,&params_iid=IID_IXAPO27Parameters;
#else
    (void)use_com;
    using Create=HRESULT(WINAPI*)(IUnknown**);auto create=(Create)GetProcAddress(provider,"CreateAudioVolumeMeter");
    hr=create?create(&object):E_NOINTERFACE;
    const IID &effect_iid=IID_IXAPO,&params_iid=IID_IXAPOParameters;
#endif
    check(SUCCEEDED(hr)&&object,"create volume meter from selected XAudio provider",hr);
    if(!object)return;
    check(owned_by(object,provider),"volume meter belongs to selected provider");
    IXAPO *effect=nullptr;IXAPOParameters *parameters=nullptr;
    hr=object->QueryInterface(effect_iid,(void**)&effect);check(SUCCEEDED(hr)&&effect,"volume meter IXAPO interface",hr);
    hr=object->QueryInterface(params_iid,(void**)&parameters);check(SUCCEEDED(hr)&&parameters,"volume meter parameter interface",hr);
    if(effect&&parameters){
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=384000;
        XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS lock{&format,256};hr=effect->LockForProcess(1,&lock,1,&lock);check(SUCCEEDED(hr),"lock volume meter for offline stereo processing",hr);
        if(SUCCEEDED(hr)){
            // This owned test array is processed offline and never sent to any
            // endpoint. It is neither microphone data nor loopback capture.
            std::vector<float> samples(512);for(unsigned i=0;i<256;i++){samples[2*i]=(i%2?-.5f:.5f);samples[2*i+1]=(i%2?.25f:-.25f);}
            auto original=samples;XAPO_PROCESS_BUFFER_PARAMETERS input{samples.data(),XAPO_BUFFER_VALID,256},output=input;
            effect->Process(1,&input,1,&output,TRUE);
            float peak[2]{},rms[2]{};MeterLevels levels{peak,rms,2};parameters->GetParameters(&levels,sizeof(levels));
            printf("Offline meter: peak=(%g,%g) rms=(%g,%g)\n",peak[0],peak[1],rms[0],rms[1]);
            check(std::fabs(peak[0]-.5f)<.0001f&&std::fabs(peak[1]-.25f)<.0001f&&std::fabs(rms[0]-.5f)<.0001f&&std::fabs(rms[1]-.25f)<.0001f,"actual volume meter computes channel peak and RMS");
            check(samples==original,"in-place volume meter preserves its offline input");
            check(effect->CalcInputFrames(256)==256&&effect->CalcOutputFrames(256)==256,"volume meter frame calculations preserve rate");
            effect->UnlockForProcess();
        }
    }
    if(parameters)parameters->Release();
    if(effect)effect->Release();
    object->Release();
}

#if XAUDIO2_VER <= 7
static void legacy_effect_com(HMODULE provider){
    const CLSID *classes[]={&CLSID_AudioVolumeMeter,&CLSID_AudioReverb};
    for(const auto clsid:classes){
        IXAPO *effect=nullptr;HRESULT hr=CoCreateInstance(*clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IXAPO27,(void**)&effect);
        check(SUCCEEDED(hr)&&effect,"CoCreateInstance creates legacy audio effect",hr);
        if(effect){check(owned_by(effect,provider),"COM effect belongs to selected provider");effect->Release();}
        MULTI_QI queries[3]={{&IID_IXAPO27,nullptr,E_FAIL},{&IID_IXAPO27Parameters,nullptr,E_FAIL},{&IID_IClassFactory,nullptr,E_FAIL}};
        hr=CoCreateInstanceEx(*clsid,nullptr,CLSCTX_INPROC_SERVER,nullptr,3,queries);
        check(hr==CO_S_NOTALLINTERFACES&&SUCCEEDED(queries[0].hr)&&queries[0].pItf&&SUCCEEDED(queries[1].hr)&&queries[1].pItf&&queries[2].hr==E_NOINTERFACE&&!queries[2].pItf,"CoCreateInstanceEx activates effect and parameter interfaces",hr);
        for(auto &query:queries)if(query.pItf){check(owned_by(query.pItf,provider),"extended COM effect belongs to selected provider");query.pItf->Release();}
    }
}

static void registered_legacy_com(HMODULE provider,const wchar_t *fixture_path,bool native,bool missing,bool empty){
    wchar_t prefix[32768];HMODULE shim=GetModuleHandleW(sizeof(void*)==4?L"winedows32.dll":L"winedows64.dll");
    bool private_prefix=shim&&GetEnvironmentVariableW(L"WINEDOWS_PREFIX",prefix,32768)>0;
    check(private_prefix,"registration fixture is running inside the prefix shim");if(!private_prefix)return;
    HMODULE marker=nullptr;
    if(missing){check(GetFileAttributesW(fixture_path)==INVALID_FILE_ATTRIBUTES,"broken registration target does not exist");}
    else{marker=LoadLibraryW(fixture_path);check(marker!=nullptr,"load private COM marker DLL",HRESULT_FROM_WIN32(GetLastError()));if(!marker)return;}
    const CLSID *classes[]={&CLSID_XAudio2,&CLSID_AudioVolumeMeter,&CLSID_AudioReverb};
    for(const auto clsid:classes){
        wchar_t guid[40];StringFromGUID2(*clsid,guid,40);std::wstring key_path=L"Software\\Classes\\CLSID\\"+std::wstring(guid);
        HKEY key=nullptr,server=nullptr;DWORD disposition=0;LSTATUS status=RegCreateKeyExW(HKEY_CURRENT_USER,key_path.c_str(),0,nullptr,0,KEY_WRITE,nullptr,&key,&disposition);
        bool created=status==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY;
        check(created,"create fresh private legacy COM registration",HRESULT_FROM_WIN32(status));
        if(!created){if(key)RegCloseKey(key);continue;}
        if(!empty){
            status=RegCreateKeyExW(key,L"InprocServer32",0,nullptr,0,KEY_WRITE,nullptr,&server,nullptr);
            if(status==ERROR_SUCCESS)status=RegSetValueExW(server,nullptr,0,REG_SZ,(const BYTE*)fixture_path,(DWORD)((wcslen(fixture_path)+1)*sizeof(wchar_t)));
            const wchar_t threading[]=L"Both";
            if(status==ERROR_SUCCESS)status=RegSetValueExW(server,L"ThreadingModel",0,REG_SZ,(const BYTE*)threading,sizeof(threading));
        }
        check(status==ERROR_SUCCESS,"prepare private COM class",HRESULT_FROM_WIN32(status));
        if(server)RegCloseKey(server);
        RegCloseKey(key);
        if(status==ERROR_SUCCESS){
            HMODULE expected=native?marker:provider;IUnknown *object=nullptr;
            HRESULT hr=CoCreateInstance(*clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IUnknown,(void**)&object);
            check(missing&&native?FAILED(hr)&&!object:SUCCEEDED(hr)&&object,missing&&native?"native mode preserves broken explicit COM registration":"activate explicitly registered legacy class",hr);
            if(object){check(owned_by(object,expected),native?"native mode preserves private COM registration":"explicit Wine mode overrides private native registration");object->Release();}
            MULTI_QI queries[2]={{&IID_IUnknown,nullptr,E_FAIL},{&IID_IClassFactory,nullptr,E_FAIL}};
            hr=CoCreateInstanceEx(*clsid,nullptr,CLSCTX_INPROC_SERVER,nullptr,2,queries);
            if(missing&&native)check(FAILED(hr)&&!queries[0].pItf&&!queries[1].pItf,"extended native activation preserves broken explicit registration",hr);
            else check(hr==CO_S_NOTALLINTERFACES&&queries[0].pItf&&SUCCEEDED(queries[0].hr)&&queries[1].hr==E_NOINTERFACE&&!queries[1].pItf,"registered class extended activation preserves partial QI",hr);
            for(auto &query:queries)if(query.pItf){check(owned_by(query.pItf,expected),"extended activation preserves registration/provider choice");query.pItf->Release();}
        }
        status=RegDeleteTreeW(HKEY_CURRENT_USER,key_path.c_str());check(status==ERROR_SUCCESS,"remove private fixture registration",HRESULT_FROM_WIN32(status));
    }
    if(marker)FreeLibrary(marker);
}
#endif

static void spatial_and_effects(const wchar_t *directory){
    std::wstring root=directory;HMODULE spatial=LoadLibraryW(L"x3daudio1_7.dll"),effects=LoadLibraryW(L"xapofx1_5.dll");
    const bool native_helpers=root==L"native";
    check(spatial!=nullptr,"load X3DAudio helper",HRESULT_FROM_WIN32(GetLastError()));
    check(effects!=nullptr,"load XAPOFX helper",HRESULT_FROM_WIN32(GetLastError()));
    if(spatial){
        check(expected_provider(spatial,native_helpers?L"native":(root+L"\\x3daudio1_7.dll").c_str()),"X3DAudio selected module path");
        using Init=void(__cdecl*)(UINT32,float,X3DAUDIO_HANDLE);using Calculate=void(__cdecl*)(const X3DAUDIO_HANDLE,const X3DAUDIO_LISTENER*,const X3DAUDIO_EMITTER*,UINT32,X3DAUDIO_DSP_SETTINGS*);
        auto initialize=(Init)GetProcAddress(spatial,"X3DAudioInitialize");auto calculate=(Calculate)GetProcAddress(spatial,"X3DAudioCalculate");check(initialize&&calculate,"X3DAudio exports available");
        if(initialize&&calculate){
            X3DAUDIO_HANDLE handle{};initialize(SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT,343.5f,handle);
            X3DAUDIO_LISTENER listener{};listener.OrientFront={0,0,1};listener.OrientTop={0,1,0};
            X3DAUDIO_EMITTER emitter{};emitter.OrientFront={0,0,1};emitter.OrientTop={0,1,0};emitter.ChannelCount=1;emitter.CurveDistanceScaler=10;emitter.DopplerScaler=1;emitter.Position={-1,0,1};
            float left[2]{},right[2]{};X3DAUDIO_DSP_SETTINGS dsp{};dsp.SrcChannelCount=1;dsp.DstChannelCount=2;dsp.pMatrixCoefficients=left;
            calculate(handle,&listener,&emitter,X3DAUDIO_CALCULATE_MATRIX|X3DAUDIO_CALCULATE_DOPPLER,&dsp);
            check(std::isfinite(left[0])&&std::isfinite(left[1])&&left[0]>left[1]&&left[0]>0,"left emitter produces finite left-biased matrix");
            check(std::fabs(dsp.DopplerFactor-1.0f)<0.00001f,"stationary source has unity Doppler");
            emitter.Position.x=1;dsp.pMatrixCoefficients=right;calculate(handle,&listener,&emitter,X3DAUDIO_CALCULATE_MATRIX|X3DAUDIO_CALCULATE_DOPPLER,&dsp);
            check(right[1]>right[0]&&std::fabs(right[1]-left[0])<0.0001f&&std::fabs(right[0]-left[1])<0.0001f,"mirrored source swaps stereo spatialization gains");
        }FreeLibrary(spatial);
    }
    if(effects){
        check(expected_provider(effects,native_helpers?L"native":(root+L"\\xapofx1_5.dll").c_str()),"XAPOFX selected module path");
        using Create=HRESULT(__cdecl*)(REFCLSID,IUnknown**);auto create=(Create)GetProcAddress(effects,"CreateFX");check(create!=nullptr,"XAPOFX CreateFX available");
        IUnknown *object=nullptr;HRESULT hr=create?create(CLSID_FXEcho27,&object):E_FAIL;check(SUCCEEDED(hr)&&object,"create actual echo effect",hr);
        if(object){
            check(owned_by(object,effects),"effect vtable belongs to selected provider");IXAPO *effect=nullptr;hr=object->QueryInterface(IID_IXAPO27,(void**)&effect);check(SUCCEEDED(hr)&&effect,"legacy IXAPO interface",hr);
            if(effect){
                XAPO_REGISTRATION_PROPERTIES *properties=nullptr;hr=effect->GetRegistrationProperties(&properties);check(SUCCEEDED(hr)&&properties&&properties->MinInputBufferCount==1&&properties->MinOutputBufferCount==1,"effect registration describes one input and output",hr);CoTaskMemFree(properties);
                WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=384000;
                XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS lock{&format,256};hr=effect->LockForProcess(1,&lock,1,&lock);check(SUCCEEDED(hr),"lock echo processor for stereo float audio",hr);
                if(SUCCEEDED(hr)){
                    std::vector<float> input(512);for(unsigned i=0;i<input.size();++i)input[i]=float(int(i%17)-8)/16.0f;auto original=input;
                    // FXEcho requires in-place buffers. Its enabled DSP is an
                    // upstream FAudio TODO; this only checks disabled bypass.
                    XAPO_PROCESS_BUFFER_PARAMETERS in{input.data(),XAPO_BUFFER_VALID,256},out=in;effect->Process(1,&in,1,&out,FALSE);
                    check(out.ValidFrameCount==256&&out.BufferFlags==XAPO_BUFFER_VALID&&input==original,"disabled in-place effect preserves offline sample data");
                    check(effect->CalcInputFrames(256)==256&&effect->CalcOutputFrames(256)==256,"effect frame calculations preserve rate");effect->UnlockForProcess();
                    if(!native_helpers)puts("LIMITATION: bundled FAPOFX echo/EQ/reverb/limiter DSP is unimplemented upstream; this is not an enabled-effect test.");
                }effect->Release();
            }object->Release();
        }FreeLibrary(effects);
    }
}

int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc<3){fputs("xaudio-probe DLL EXPECTED_PATH_OR_native [HELPER_DIRECTORY_OR_-] [com|registered COM_FIXTURE_DLL]\n",stderr);return 2;}
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool uninitialize=SUCCEEDED(hr);check(uninitialize,"initialize COM",hr);if(FAILED(hr))return 1;
    HMODULE provider=LoadLibraryW(argv[1]);check(provider!=nullptr,"load requested XAudio2 provider",HRESULT_FROM_WIN32(GetLastError()));
    if(!provider){CoUninitialize();return 1;}check(expected_provider(provider,argv[2]),"XAudio2 module matches selected provider");
    const bool use_com=argc>4&&!wcscmp(argv[4],L"com");
    IXAudio2 *engine=nullptr;
#if XAUDIO2_VER <= 7
    if(argc>5&&(!wcscmp(argv[4],L"registered")||!wcscmp(argv[4],L"registered-missing")||!wcscmp(argv[4],L"registered-empty"))){
        registered_legacy_com(provider,argv[5],!wcscmp(argv[2],L"native"),wcscmp(argv[4],L"registered")!=0,!wcscmp(argv[4],L"registered-empty"));
        FreeLibrary(provider);CoUninitialize();printf("Legacy COM registration: %d failure(s)\n",failures);return failures?1:0;
    }
    if(use_com){
        // Exercise ordinary application COM activation without registering the
        // Wine DLL in either the host registry or the private prefix registry.
        hr=CoCreateInstance(CLSID_XAudio2,nullptr,CLSCTX_INPROC_SERVER,IID_IXAudio2,(void**)&engine);
        check(SUCCEEDED(hr)&&engine,"CoCreateInstance selects the legacy XAudio provider",hr);
        MULTI_QI queries[2]={{&IID_IXAudio2,nullptr,E_FAIL},{&IID_IClassFactory,nullptr,E_FAIL}};
        HRESULT extended=CoCreateInstanceEx(CLSID_XAudio2,nullptr,CLSCTX_INPROC_SERVER,nullptr,2,queries);
        check(extended==CO_S_NOTALLINTERFACES&&SUCCEEDED(queries[0].hr)&&queries[0].pItf&&queries[1].hr==E_NOINTERFACE&&!queries[1].pItf,"CoCreateInstanceEx preserves partial interface results",extended);
        if(queries[0].pItf){check(owned_by(queries[0].pItf,provider),"extended COM object belongs to selected provider");queries[0].pItf->Release();}
        if(queries[1].pItf)queries[1].pItf->Release();
    }else{
    using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);auto factory=(Factory)GetProcAddress(provider,"DllGetClassObject");IClassFactory *object=nullptr;
    hr=factory?factory(CLSID_XAudio2,IID_IClassFactory,(void**)&object):E_NOINTERFACE;check(SUCCEEDED(hr)&&object,"obtain legacy XAudio2 class factory",hr);
    if(object){hr=object->CreateInstance(nullptr,IID_IXAudio2,(void**)&engine);object->Release();check(SUCCEEDED(hr)&&engine,"create legacy XAudio2 interface",hr);}
    }
    if(engine){hr=engine->Initialize(0,XAUDIO2_DEFAULT_PROCESSOR);check(SUCCEEDED(hr),"initialize legacy XAudio engine",hr);}
#else
    using Create=HRESULT(WINAPI*)(IXAudio2**,UINT32,XAUDIO2_PROCESSOR);auto create=(Create)GetProcAddress(provider,"XAudio2Create");check(create!=nullptr,"XAudio2Create export available");
#ifdef WINEDOWS_STATIC_XAUDIO_IMPORT
    const char *dll=XAUDIO2_VER==8?"xaudio2_8.dll":"xaudio2_9.dll";check(imported_from("XAudio2Create",dll,provider),"startup import binds to the selected XAudio2 export");hr=XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR);
#else
    hr=create?create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR):E_NOINTERFACE;
#endif
    check(SUCCEEDED(hr)&&engine,"create XAudio2 engine",hr);
#endif
    if(engine){check(owned_by(engine,provider),"engine vtable belongs to selected provider");if(SUCCEEDED(hr))stream(engine,provider);engine->Release();}
    volume_meter(provider,use_com);
#if XAUDIO2_VER <= 7
    if(use_com)legacy_effect_com(provider);
#endif
    if(argc>3&&wcscmp(argv[3],L"-"))spatial_and_effects(argv[3]);
    FreeLibrary(provider);if(uninitialize)CoUninitialize();
    printf("XAudio2.%d %zu-bit: %d failure(s); silent muted render, no capture\n",XAUDIO2_VER,sizeof(void*)*8,failures);return failures?1:0;
}
