// SPDX-License-Identifier: LGPL-2.1-or-later
// Real Wine/native XACT engine tests. Every source is muted before playback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <xact3.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include "xact-fixture-data.h"

static volatile LONG notifications[32];
static volatile LONG callback_errors;
static int context_marker;
static IXACT3Wave *expected_wave;
static IXACT3WaveBank *expected_bank;
static XACTINDEX expected_index;
static IXACT3Wave *race_wave;
static HANDLE race_entered,race_destroy_started;
static volatile LONG race_query_completed;
static volatile LONG bank_play_capture;
static PVOID volatile bank_play_wave;
static IXACT3WaveBank *managed_bank;
static void WINAPI notify(const XACT_NOTIFICATION *event){
    if(!event||event->type>=32||event->pvContext!=&context_marker){InterlockedIncrement(&callback_errors);return;}
    if(expected_wave&&(event->type==XACTNOTIFICATIONTYPE_WAVEPLAY||event->type==XACTNOTIFICATIONTYPE_WAVESTOP||event->type==XACTNOTIFICATIONTYPE_WAVEDESTROYED)){
        if(event->wave.pWave!=expected_wave||event->wave.pWaveBank!=expected_bank||event->wave.waveIndex!=expected_index||event->wave.pCue||event->wave.pSoundBank)
            InterlockedIncrement(&callback_errors);
    }
    if(bank_play_capture&&event->type==XACTNOTIFICATIONTYPE_WAVEPLAY)InterlockedExchangePointer(&bank_play_wave,event->wave.pWave);
    if(managed_bank&&(event->type==XACTNOTIFICATIONTYPE_WAVEPLAY||event->type==XACTNOTIFICATIONTYPE_WAVESTOP||event->type==XACTNOTIFICATIONTYPE_WAVEDESTROYED)){
        DWORD value=0;
        if(!event->wave.pWave||event->wave.pWaveBank!=managed_bank||event->wave.waveIndex||FAILED(event->wave.pWave->GetState(&value)))InterlockedIncrement(&callback_errors);
    }
    if(event->type==XACTNOTIFICATIONTYPE_WAVESTOP&&event->wave.pWave==race_wave&&race_wave){
        SetEvent(race_entered);
        if(WaitForSingleObject(race_destroy_started,2000)!=WAIT_OBJECT_0)InterlockedIncrement(&callback_errors);
        Sleep(50);DWORD value=0;
        if(FAILED(event->wave.pWave->GetState(&value))||!(value&XACT_STATE_STOPPED))InterlockedIncrement(&callback_errors);
        InterlockedIncrement(&race_query_completed);
    }
    InterlockedIncrement(&notifications[event->type]);
}
static void require(bool value,const char *message){printf("%s %s\n",value?"PASS":"FAIL",message);if(!value)throw std::runtime_error(message);}
static void hrcheck(HRESULT hr,const char *message){printf("%s %s hr=%08lx\n",SUCCEEDED(hr)?"PASS":"FAIL",message,(unsigned long)hr);if(FAILED(hr))throw std::runtime_error(message);}
static std::vector<BYTE> read_file(const wchar_t *path){
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(file!=INVALID_HANDLE_VALUE,"open staged wave bank");
    LARGE_INTEGER size{};BOOL valid=GetFileSizeEx(file,&size);if(!valid||size.QuadPart<52||size.QuadPart>256*1024*1024){CloseHandle(file);throw std::runtime_error("invalid wave bank size");}
    std::vector<BYTE> data((size_t)size.QuadPart);DWORD done=0;valid=ReadFile(file,data.data(),(DWORD)data.size(),&done,nullptr);CloseHandle(file);require(valid&&done==data.size(),"read complete staged wave bank");return data;
}
struct Engine {
    IXACT3Engine *value=nullptr;bool initialized=false;
    ~Engine(){if(value){if(initialized)value->Shutdown();value->Release();}}
};
struct PrivateRegistration {
    bool active=false;
    const wchar_t *key=L"Software\\Classes\\CLSID\\{bcc782bc-6492-4c22-8c35-f5d72fe73c6e}";
    void create(const wchar_t *provider,bool empty){
        require(GetModuleHandleW(L"winedows32.dll")||GetModuleHandleW(L"winedows64.dll"),"private COM fixture runs only in a prefix");
        HKEY entry=nullptr;DWORD disposition=0;
        require(RegCreateKeyExW(HKEY_CURRENT_USER,key,0,nullptr,0,KEY_WRITE,nullptr,&entry,&disposition)==ERROR_SUCCESS,"create private COM class");
        RegCloseKey(entry);require(disposition==REG_CREATED_NEW_KEY,"private COM fixture does not overwrite registration");active=true;
        if(!empty){std::wstring server=std::wstring(key)+L"\\InprocServer32";require(RegCreateKeyExW(HKEY_CURRENT_USER,server.c_str(),0,nullptr,0,KEY_WRITE,nullptr,&entry,nullptr)==ERROR_SUCCESS,"create private in-process server");
            LSTATUS status=RegSetValueExW(entry,nullptr,0,REG_SZ,(const BYTE*)provider,(DWORD)((wcslen(provider)+1)*sizeof(wchar_t)));
            if(status==ERROR_SUCCESS)status=RegSetValueExW(entry,L"ThreadingModel",0,REG_SZ,(const BYTE*)L"Both",5*sizeof(wchar_t));RegCloseKey(entry);require(status==ERROR_SUCCESS,"write private server path and threading model");}
    }
    ~PrivateRegistration(){if(active)RegDeleteTreeW(HKEY_CURRENT_USER,key);}
};
template<class Object>static DWORD state(Object *object){DWORD value=0;HRESULT hr=object->GetState(&value);if(FAILED(hr))throw std::runtime_error("GetState failed");return value;}
template<class Object>static bool wait_state(IXACT3Engine *engine,Object *object,DWORD bits,DWORD timeout){
    ULONGLONG until=GetTickCount64()+timeout;do{engine->DoWork();if((state(object)&bits)==bits)return true;Sleep(5);}while(GetTickCount64()<until);return false;
}
static void pump(IXACT3Engine *engine,DWORD duration){ULONGLONG end=GetTickCount64()+duration;do{engine->DoWork();Sleep(5);}while(GetTickCount64()<end);}
static void register_event(IXACT3Engine *engine,XACTNOTIFICATIONTYPE type){
    XACT_NOTIFICATION_DESCRIPTION desc{};desc.type=type;desc.flags=XACT_FLAG_NOTIFICATION_PERSIST;desc.cueIndex=XACTINDEX_INVALID;desc.waveIndex=XACTINDEX_INVALID;desc.pvContext=&context_marker;
    hrcheck(engine->RegisterNotification(&desc),"register persistent playback notification");
}
static void wave_test(IXACT3Engine *engine,IXACT3WaveBank *bank,XACTINDEX index,bool pause){
    IXACT3Wave *wave=nullptr;hrcheck(bank->Prepare(index,0,0,0,&wave),"prepare real wave-bank entry");require(wave!=nullptr,"prepared wave interface exists");
    try{
        require(wait_state(engine,wave,XACT_STATE_PREPARED,3000),"wave finishes asynchronous preparation");
        hrcheck(wave->SetVolume(0.0f),"mute wave before playback");
        XACT_WAVE_INSTANCE_PROPERTIES properties{};hrcheck(wave->GetProperties(&properties),"inspect wave format and duration");
        printf("WAVE index=%u format=%u rate=%u channels=%u samples=%lu\n",index,properties.properties.format.wFormatTag,properties.properties.format.nSamplesPerSec,properties.properties.format.nChannels,properties.properties.durationInSamples);
        require(properties.properties.durationInSamples>0&&properties.properties.format.nSamplesPerSec==(index==1?44080u:44100u),"wave bank has real decoded timing metadata");
        LONG plays=notifications[XACTNOTIFICATIONTYPE_WAVEPLAY],stops=notifications[XACTNOTIFICATIONTYPE_WAVESTOP],destroys=notifications[XACTNOTIFICATIONTYPE_WAVEDESTROYED];
        expected_wave=wave;expected_bank=bank;expected_index=index;
        ULONGLONG start=GetTickCount64();hrcheck(wave->Play(),"play muted wave through real engine");require(wait_state(engine,wave,XACT_STATE_PLAYING,2000),"wave enters playing state");
        if(pause){
            hrcheck(wave->Pause(TRUE),"pause playing wave");require(wait_state(engine,wave,XACT_STATE_PAUSED,1000),"wave reports paused state");
            DWORD held_for=properties.properties.durationInSamples*1000/properties.properties.format.nSamplesPerSec+150;
            pump(engine,held_for);DWORD held=state(wave);require((held&XACT_STATE_PAUSED)&&!(held&XACT_STATE_STOPPED),"paused wave stays incomplete beyond its normal duration");
            hrcheck(wave->Pause(FALSE),"resume wave clock");
        }
        require(wait_state(engine,wave,XACT_STATE_STOPPED,5000),"wave completes after its audio clock advances");pump(engine,40);
        printf("WAVE_COMPLETE index=%u elapsed_ms=%llu play_notifications=%ld stop_notifications=%ld\n",index,GetTickCount64()-start,notifications[XACTNOTIFICATIONTYPE_WAVEPLAY]-plays,notifications[XACTNOTIFICATIONTYPE_WAVESTOP]-stops);
        require(notifications[XACTNOTIFICATIONTYPE_WAVEPLAY]==plays+1&&notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops+1,"exactly one wave play/stop callback delivered");
        hrcheck(wave->Destroy(),"destroy completed wave");wave=nullptr;expected_wave=nullptr;pump(engine,30);
        require(notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops+1,"destroy does not repeat completed wave stop");
        require(notifications[XACTNOTIFICATIONTYPE_WAVEDESTROYED]==destroys+1,"wave destroyed notification delivered once");
    }catch(...){if(wave){wave->Stop(XACT_FLAG_STOP_IMMEDIATE);wave->Destroy();}expected_wave=nullptr;throw;}
}
static DWORD WINAPI destroy_wave(void *value){if(WaitForSingleObject(race_entered,6000)!=WAIT_OBJECT_0)return (DWORD)HRESULT_FROM_WIN32(ERROR_TIMEOUT);SetEvent(race_destroy_started);return (DWORD)((IXACT3Wave*)value)->Destroy();}
static void reentrant_completion_test(IXACT3Engine *engine,IXACT3WaveBank *bank){
    IXACT3Wave *wave=nullptr;hrcheck(bank->Prepare(0,0,0,0,&wave),"prepare callback reentry wave");hrcheck(wave->SetVolume(0),"mute callback reentry wave");require(wait_state(engine,wave,XACT_STATE_PREPARED,3000),"reentry wave preparation completes");
    race_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);race_destroy_started=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(race_entered&&race_destroy_started,"create callback coordination events");
    race_wave=wave;expected_wave=wave;expected_bank=bank;expected_index=0;race_query_completed=0;
    HANDLE worker=CreateThread(nullptr,0,destroy_wave,wave,0,nullptr);require(worker!=nullptr,"start concurrent wave destroy");
    hrcheck(wave->Play(),"play callback reentry wave");ULONGLONG until=GetTickCount64()+7000;
    while(WaitForSingleObject(worker,0)==WAIT_TIMEOUT&&GetTickCount64()<until)pump(engine,5);
    require(WaitForSingleObject(worker,0)==WAIT_OBJECT_0,"callback GetState and concurrent Destroy finish without deadlock");DWORD result=0;GetExitCodeThread(worker,&result);CloseHandle(worker);hrcheck((HRESULT)result,"concurrent destroy succeeds");
    require(race_query_completed==1,"completion callback safely queries stopped wave once");race_wave=nullptr;expected_wave=nullptr;CloseHandle(race_entered);CloseHandle(race_destroy_started);
}
static void wavebank_play_test(IXACT3Engine *engine,const std::vector<BYTE>& original){
    auto silent=original;auto *header=(WAVEBANKHEADER*)silent.data();auto *entry=(WAVEBANKENTRY*)(silent.data()+header->Segments[WAVEBANK_SEGIDX_ENTRYMETADATA].dwOffset);
    size_t offset=(size_t)header->Segments[WAVEBANK_SEGIDX_ENTRYWAVEDATA].dwOffset+entry->PlayRegion.dwOffset;
    require(entry->Format.wFormatTag==WAVEBANKMINIFORMAT_TAG_PCM&&!entry->Format.wBitsPerSample&&offset<=silent.size()&&entry->PlayRegion.dwLength<=silent.size()-offset,"validate 8-bit PCM silence bounds");
    memset(silent.data()+offset,128,entry->PlayRegion.dwLength);
    // This copy uses the buffered API even when derived from a streaming bank.
    auto *data=(WAVEBANKDATA*)(silent.data()+header->Segments[WAVEBANK_SEGIDX_BANKDATA].dwOffset);data->dwFlags&=~WAVEBANK_TYPE_STREAMING;
    IXACT3WaveBank *bank=nullptr;hrcheck(engine->CreateInMemoryWaveBank(silent.data(),(DWORD)silent.size(),0,0,&bank),"create digital-silence bank for immediate Play");
    IXACT3Wave *wave=nullptr;bank_play_wave=nullptr;bank_play_capture=1;LONG plays=notifications[XACTNOTIFICATIONTYPE_WAVEPLAY];
    hrcheck(bank->Play(0,0,0,0,&wave),"WaveBank Play returns a live wave handle");
    ULONGLONG end=GetTickCount64()+1000;while(notifications[XACTNOTIFICATIONTYPE_WAVEPLAY]==plays&&GetTickCount64()<end)pump(engine,5);
    bank_play_capture=0;require(bank_play_wave==wave&&wave,"WaveBank Play callback identifies returned wrapper");
    hrcheck(wave->SetVolume(0),"mute already-silent Play wave");require(wait_state(engine,wave,XACT_STATE_STOPPED,5000),"immediate Play silence completes");hrcheck(wave->Destroy(),"destroy immediate Play wave");hrcheck(bank->Destroy(),"destroy digital-silence bank");
}
static void no_handle_test(IXACT3Engine *engine,IXACT3WaveBank *&bank,const std::vector<BYTE>& data,bool destroy_notification,unsigned object_notification){
    auto *header=(const WAVEBANKHEADER*)data.data();auto *entry=(const WAVEBANKENTRY*)(data.data()+header->Segments[WAVEBANK_SEGIDX_ENTRYMETADATA].dwOffset);
    size_t offset=(size_t)header->Segments[WAVEBANK_SEGIDX_ENTRYWAVEDATA].dwOffset+entry->PlayRegion.dwOffset;
    require(entry->Format.wFormatTag==WAVEBANKMINIFORMAT_TAG_PCM&&!entry->Format.wBitsPerSample&&offset<=data.size()&&entry->PlayRegion.dwLength<=data.size()-offset,"validate no-handle PCM bounds");
    for(size_t i=0;i<entry->PlayRegion.dwLength;i++)if(data[offset+i]!=128)throw std::runtime_error("no-handle bank must contain digital silence");
    managed_bank=bank;
    for(unsigned mode=0;mode<3;mode++){
        LONG plays=notifications[XACTNOTIFICATIONTYPE_WAVEPLAY],stops=notifications[XACTNOTIFICATIONTYPE_WAVESTOP],destroys=notifications[XACTNOTIFICATIONTYPE_WAVEDESTROYED];
        bank_play_capture=1;bank_play_wave=nullptr;hrcheck(bank->Play(0,0,0,0,nullptr),"WaveBank Play accepts omitted output handle");
        ULONGLONG until=GetTickCount64()+2000;while(notifications[XACTNOTIFICATIONTYPE_WAVEPLAY]==plays&&GetTickCount64()<until)pump(engine,5);
        require(notifications[XACTNOTIFICATIONTYPE_WAVEPLAY]==plays+1,"no-handle wave starts once");bank_play_capture=0;
        printf("NO_HANDLE mode=%u callback_wave=%p\n",mode,bank_play_wave);
        if(object_notification){XACT_NOTIFICATION_DESCRIPTION desc{};desc.type=XACTNOTIFICATIONTYPE_WAVEDESTROYED;desc.pWave=(IXACT3Wave*)bank_play_wave;desc.pvContext=&context_marker;
            hrcheck(engine->RegisterNotification(&desc),"register per-wave destruction callback");if(object_notification==2)hrcheck(engine->UnRegisterNotification(&desc),"unregister per-wave destruction callback");}
        if(mode==1)hrcheck(bank->Stop(0,XACT_FLAG_STOP_IMMEDIATE),"cancel no-handle wave by bank index");
        if(mode==2){hrcheck(bank->Destroy(),"destroy bank with active no-handle wave");bank=nullptr;}
        until=GetTickCount64()+5000;while(notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops&&GetTickCount64()<until)pump(engine,5);pump(engine,100);
        require(notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops+1,"no-handle wave completes exactly once");
        printf("NO_HANDLE_LIFETIME mode=%u destroyed=%ld\n",mode,notifications[XACTNOTIFICATIONTYPE_WAVEDESTROYED]-destroys);
        require(notifications[XACTNOTIFICATIONTYPE_WAVEDESTROYED]==destroys+((destroy_notification||object_notification==1)?1:0),"managed wave destruction respects client notification subscription");
        if(bank){hrcheck(bank->Stop(0,XACT_FLAG_STOP_IMMEDIATE),"repeat no-handle bank cancellation safely");pump(engine,30);require(notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops+1,"bank cancellation does not duplicate completed stop");}
    }
    managed_bank=nullptr;
}
static void stop_test(IXACT3Engine *engine,IXACT3WaveBank *bank,unsigned mode){
    IXACT3Wave *wave=nullptr;hrcheck(bank->Prepare(0,0,0,mode==2?2:0,&wave),"prepare explicit-stop wave");hrcheck(wave->SetVolume(0),"mute explicit-stop wave");
    require(wait_state(engine,wave,XACT_STATE_PREPARED,3000),"explicit-stop wave preparation completes");
    expected_wave=wave;expected_bank=bank;expected_index=0;LONG stops=notifications[XACTNOTIFICATIONTYPE_WAVESTOP];
    hrcheck(wave->Play(),"start explicit-stop wave");require(wait_state(engine,wave,XACT_STATE_PLAYING,1000),"explicit-stop wave starts");
    if(mode==1){hrcheck(wave->Destroy(),"destroy actively playing standalone wave");wave=nullptr;}
    else{hrcheck(wave->Stop(mode==2?XACT_FLAG_STOP_RELEASE:XACT_FLAG_STOP_IMMEDIATE),mode==2?"release looping standalone wave":"stop standalone wave immediately");require(wait_state(engine,wave,XACT_STATE_STOPPED,5000),"explicit stop completes");hrcheck(wave->Stop(XACT_FLAG_STOP_IMMEDIATE),"repeat completed stop safely");}
    pump(engine,80);require(notifications[XACTNOTIFICATIONTYPE_WAVESTOP]==stops+1,"explicit stop/destroy delivers one completion");
    if(wave)hrcheck(wave->Destroy(),"destroy explicitly stopped wave");expected_wave=nullptr;
}
static DWORD WINAPI watchdog(void*){Sleep(30000);TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc<3)return 2;HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    bool com=false,extended=false,streaming=false,expect_failure=false,no_handle=false,destroy_notification=true,unregister_destroy=false;unsigned registration=0,object_notification=0;
    for(int i=3;i<argc;i++){if(!wcscmp(argv[i],L"--com"))com=true;else if(!wcscmp(argv[i],L"--com-ex"))com=extended=true;else if(!wcscmp(argv[i],L"--streaming"))streaming=true;
        else if(!wcscmp(argv[i],L"--private-empty"))registration=1;else if(!wcscmp(argv[i],L"--private-missing"))registration=2;else if(!wcscmp(argv[i],L"--private-native"))registration=3;
        else if(!wcscmp(argv[i],L"--expect-activation-failure"))expect_failure=true;else if(!wcscmp(argv[i],L"--no-handle-only"))no_handle=true;else if(!wcscmp(argv[i],L"--no-destroy-notification"))destroy_notification=false;
        else if(!wcscmp(argv[i],L"--unregister-destroy"))unregister_destroy=true;else if(!wcscmp(argv[i],L"--object-destroy")){object_notification=1;destroy_notification=false;}
        else if(!wcscmp(argv[i],L"--unregister-object-destroy")){object_notification=2;destroy_notification=false;}else return 2;}
    HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(co))return 1;int result=1;HMODULE module=nullptr;HANDLE stream=INVALID_HANDLE_VALUE;
    try{
        PrivateRegistration private_class;if(registration)private_class.create(registration==3?argv[1]:L"C:\\missing-xact-fixture.dll",registration==1);
        Engine engine;
        if(com){
            HRESULT hr;
            if(extended){MULTI_QI q[2]={{&IID_IXACT3Engine,nullptr,0},{&IID_IUnknown,nullptr,0}};hr=CoCreateInstanceEx(CLSID_XACTEngine,nullptr,CLSCTX_INPROC_SERVER,nullptr,2,q);engine.value=(IXACT3Engine*)q[0].pItf;if(q[1].pItf)q[1].pItf->Release();}
            else hr=CoCreateInstance(CLSID_XACTEngine,nullptr,CLSCTX_INPROC_SERVER,IID_IXACT3Engine,(void**)&engine.value);
            if(expect_failure){printf("ACTIVATION hr=%08lx\n",(unsigned long)hr);require(FAILED(hr)&&!engine.value,"explicit private registration prevents native fallback");CoUninitialize();return 0;}
            hrcheck(hr,extended?"activate XACT through CoCreateInstanceEx":"activate XACT through CoCreateInstance");
        }else{
            module=LoadLibraryW(argv[1]);require(module!=nullptr,"load requested XACT provider");
            auto factory=(HRESULT(WINAPI*)(REFCLSID,REFIID,void**))GetProcAddress(module,"DllGetClassObject");require(factory!=nullptr,"resolve real XACT COM class factory");
            IClassFactory *object=nullptr;hrcheck(factory(CLSID_XACTEngine,IID_IClassFactory,(void**)&object),"obtain XACT3.7 factory");HRESULT hr=object->CreateInstance(nullptr,IID_IXACT3Engine,(void**)&engine.value);object->Release();hrcheck(hr,"instantiate genuine XACT3.7 engine");
        }
        require(engine.value!=nullptr,"engine interface returned");
        HMODULE provider=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(*(void***)engine.value)[0],&provider);
        wchar_t actual[32767]{};DWORD length=GetModuleFileNameW(provider,actual,32767);require(length&&length<32767,"identify engine implementation");wprintf(L"PROVIDER %ls\n",actual);
        HANDLE expected=CreateFileW(argv[1],FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr),found=CreateFileW(actual,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        FILE_ID_INFO a{},b{};bool identical=expected!=INVALID_HANDLE_VALUE&&found!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(expected,FileIdInfo,&a,sizeof(a))&&GetFileInformationByHandleEx(found,FileIdInfo,&b,sizeof(b))&&a.VolumeSerialNumber==b.VolumeSerialNumber&&!memcmp(&a.FileId,&b.FileId,sizeof(a.FileId));if(expected!=INVALID_HANDLE_VALUE)CloseHandle(expected);if(found!=INVALID_HANDLE_VALUE)CloseHandle(found);require(identical,"engine file identity matches selected provider");
        auto wave_data=read_file(argv[2]);std::vector<BYTE> sound(soundbank_data,soundbank_data+sizeof(soundbank_data));
        XACT_RUNTIME_PARAMETERS params{};params.pGlobalSettingsBuffer=(void*)global_settings_data;params.globalSettingsBufferSize=sizeof(global_settings_data);params.fnNotificationCallback=notify;
        hrcheck(engine.value->Initialize(&params),"initialize real XACT settings and Windows endpoint");engine.initialized=true;
        WAVEFORMATEXTENSIBLE mix{};hrcheck(engine.value->GetFinalMixFormat(&mix),"read final audio mix format");require(mix.Format.nChannels&&mix.Format.nSamplesPerSec,"audio endpoint has real mix format");
        XACTCATEGORY global=engine.value->GetCategory("Global");require(global!=XACTCATEGORY_INVALID,"resolve root audio category");hrcheck(engine.value->SetVolume(global,0.0f),"mute complete category hierarchy before cues");
        for(auto type:{XACTNOTIFICATIONTYPE_WAVEPLAY,XACTNOTIFICATIONTYPE_WAVESTOP,XACTNOTIFICATIONTYPE_WAVEDESTROYED,XACTNOTIFICATIONTYPE_CUEPLAY,XACTNOTIFICATIONTYPE_CUESTOP})if(type!=XACTNOTIFICATIONTYPE_WAVEDESTROYED||destroy_notification)register_event(engine.value,type);
        if(unregister_destroy){XACT_NOTIFICATION_DESCRIPTION desc{};desc.type=XACTNOTIFICATIONTYPE_WAVEDESTROYED;desc.flags=XACT_FLAG_NOTIFICATION_PERSIST;desc.cueIndex=XACTINDEX_INVALID;desc.waveIndex=XACTINDEX_INVALID;desc.pvContext=&context_marker;hrcheck(engine.value->UnRegisterNotification(&desc),"unregister persistent wave destruction callback");destroy_notification=false;}
        XACTVARIABLEINDEX variable=engine.value->GetGlobalVariableIndex("v1");require(variable!=XACTVARIABLEINDEX_INVALID,"resolve bank global variable");hrcheck(engine.value->SetGlobalVariable(variable,27.0f),"set global sound variable");XACTVARIABLEVALUE value=0;hrcheck(engine.value->GetGlobalVariable(variable,&value),"read global sound variable");require(value==27.0f,"global variable round trip");
        IXACT3WaveBank *bank=nullptr;
        if(streaming){stream=CreateFileW(argv[2],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);require(stream!=INVALID_HANDLE_VALUE,"open staged bank for overlapped streaming");XACT_STREAMING_PARAMETERS p{};p.file=stream;p.packetSize=2;hrcheck(engine.value->CreateStreamingWaveBank(&p,&bank),"create streaming wave bank");require(wait_state(engine.value,bank,XACT_STATE_PREPARED,3000),"streaming bank finishes preparation");}
        else hrcheck(engine.value->CreateInMemoryWaveBank(wave_data.data(),(DWORD)wave_data.size(),0,0,&bank),"create in-memory wave bank");
        XACTINDEX waves=0;hrcheck(bank->GetNumWaves(&waves),"enumerate wave-bank entries");require(waves==3,"all PCM/ADPCM/xWMA bank entries visible");
        if(no_handle){no_handle_test(engine.value,bank,wave_data,destroy_notification,object_notification);require(!callback_errors,"no-handle callback contexts and interfaces remain intact");hrcheck(engine.value->Shutdown(),"shut down no-handle engine");engine.initialized=false;puts("PASS XACT no-handle playback, cancellation and lifetime");return 0;}
        for(XACTINDEX i=0;i<waves;i++)wave_test(engine.value,bank,i,i==0);
        stop_test(engine.value,bank,0);stop_test(engine.value,bank,1);stop_test(engine.value,bank,2);
        reentrant_completion_test(engine.value,bank);
        IXACT3SoundBank *sounds=nullptr;hrcheck(engine.value->CreateSoundBank(sound.data(),(DWORD)sound.size(),0,0,&sounds),"create real sound bank");XACTINDEX cues=0;hrcheck(sounds->GetNumCues(&cues),"enumerate real cues");require(cues==3,"all three sound-bank cues visible");
        XACTINDEX cue_index=sounds->GetCueIndex("c1");require(cue_index!=XACTINDEX_INVALID,"resolve cue by friendly name");IXACT3Cue *cue=nullptr;hrcheck(sounds->Prepare(cue_index,0,0,&cue),"prepare sound-bank cue");
        LONG starts=notifications[XACTNOTIFICATIONTYPE_CUEPLAY],stops=notifications[XACTNOTIFICATIONTYPE_CUESTOP];hrcheck(cue->Play(),"play muted cue through sound bank");require(wait_state(engine.value,cue,XACT_STATE_STOPPED,5000),"cue completes");pump(engine.value,40);
        require(notifications[XACTNOTIFICATIONTYPE_CUEPLAY]>starts&&notifications[XACTNOTIFICATIONTYPE_CUESTOP]>stops,"real cue start/stop notifications delivered");
        hrcheck(cue->Destroy(),"destroy completed cue");hrcheck(sounds->Destroy(),"destroy sound bank");hrcheck(bank->Destroy(),"destroy wave bank");wavebank_play_test(engine.value,wave_data);require(!callback_errors,"callback contexts and payloads remain intact");
        hrcheck(engine.value->Shutdown(),"shut down engine and worker threads");engine.initialized=false;puts("PASS XACT engine playback, timing and notifications");result=0;
    }catch(const std::exception &error){fprintf(stderr,"FAIL XACT %s\n",error.what());}
    if(stream!=INVALID_HANDLE_VALUE)CloseHandle(stream);if(module)FreeLibrary(module);CoUninitialize();return result;
}
