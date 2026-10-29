// SPDX-License-Identifier: LGPL-2.1-or-later
// Diagnostic for Blossom Tales II's actual FAudio 22.07 / SDL 2.24 binaries.
// Records application render-callback timing, not hardware underrun counters.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include "game-audio-wasapi.h"
#include "game-audio-render.h"
#include "../../libs/faudio/include/FAudio.h"
#include "../../libs/faudio/include/FACT.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// These public layouts were checked against FAudio tag 22.07. Later declarations
// in the bundled source header are unused. Calls resolve only the copied DLL.
static_assert(sizeof(void*)==8,"The staged Blossom audio libraries are x64");
static_assert(sizeof(FAudioBuffer)==44&&sizeof(FAudioVoiceState)==20,"FAudio 22.07 packed ABI");
static_assert(sizeof(FACTRuntimeParameters)==80,"FAudio 22.07 XACT ABI");
struct SdlSpec {int freq;uint16_t format;uint8_t channels,silence;uint16_t samples,padding;uint32_t size;void(__cdecl *callback)(void*,uint8_t*,int);void *userdata;};
struct SdlVersion {uint8_t major,minor,patch;};
static_assert(sizeof(SdlSpec)==32,"SDL 2.24 x64 audio spec ABI");
using SdlOpen=uint32_t(__cdecl*)(const char*,int,const SdlSpec*,SdlSpec*,int);

static void require(bool ok,const char *what){if(!ok)throw std::runtime_error(what);}
static void success(uint32_t code,const char *what){if(code)throw std::runtime_error(std::string(what)+" error="+std::to_string(code));}
static int64_t tick(){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> value(32768);DWORD n=GetModuleFileNameW(module,value.data(),(DWORD)value.size());return n&&n<value.size()?std::wstring(value.data(),n):L"";}
static std::string utf8(const std::wstring &s){int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string result(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,s.c_str(),(int)s.size(),result.data(),n,nullptr,nullptr);return result;}
static std::string json(const std::string &s){std::string result="\"";for(unsigned char c:s){if(c=='\\'||c=='\"'){result+='\\';result+=(char)c;}else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);result+=b;}else result+=(char)c;}return result+'"';}
template<class T> static T load(HMODULE dll,const char *name){auto fn=(T)GetProcAddress(dll,name);if(!fn)throw std::runtime_error(std::string("Missing export ")+name);return fn;}
#define AUDIO_EXPORTS(X) \
 X(FAudioLinkedVersion) X(FAudioCreate) X(FAudio_Release) X(FAudio_RegisterForCallbacks) X(FAudio_UnregisterForCallbacks) \
 X(FAudio_CreateMasteringVoice) X(FAudio_CreateSourceVoice) X(FAudio_StartEngine) X(FAudio_StopEngine) X(FAudio_GetProcessingQuantum) \
 X(FAudio_GetPerformanceData) X(FAudioVoice_SetVolume) X(FAudioVoice_GetVolume) X(FAudioVoice_GetVoiceDetails) X(FAudioVoice_DestroyVoice) \
 X(FAudioSourceVoice_SubmitSourceBuffer) X(FAudioSourceVoice_Start) X(FAudioSourceVoice_Stop) X(FAudioSourceVoice_GetState) \
 X(FACTCreateEngine) X(FACTAudioEngine_Initialize) X(FACTAudioEngine_ShutDown) X(FACTAudioEngine_Release) X(FACTAudioEngine_DoWork) \
 X(FACTAudioEngine_CreateSoundBank) X(FACTAudioEngine_CreateInMemoryWaveBank) X(FACTSoundBank_GetCueIndex) X(FACTSoundBank_Play) X(FACTCue_GetState)
struct Api {
#define DECLARE(name) decltype(&::name) name;
 AUDIO_EXPORTS(DECLARE)
#undef DECLARE
 explicit Api(HMODULE dll){
#define LOAD(name) name=load<decltype(name)>(dll,#name);
 AUDIO_EXPORTS(LOAD)
#undef LOAD
 }
};
struct Row {int64_t start,end;uint32_t length,thread;bool injected;};
struct Trace {
    Row rows[32768]{};
    std::atomic<unsigned> used{0},dropped{0},opens{0},capture_attempts{0};
    std::atomic<int64_t> begin{0},finish{0},inject_after{0};
    std::atomic<bool> injected{false};
    unsigned stall_ms=0;SdlOpen open=nullptr;SdlSpec requested{},obtained{};
    void(__cdecl *callback)(void*,uint8_t*,int)=nullptr;void *userdata=nullptr;
};
// Lifetime extends to process reclamation, including abnormal fixture exits.
static Trace &trace=*new Trace;
struct WaveRow {int64_t at;uint32_t kind,bytes;int pending;};
struct WaveTrace {
    WaveRow rows[32768]{};std::atomic<unsigned> used{0},dropped{0};
    std::atomic<int> pending{0};std::atomic<unsigned> opens{0},errors{0};
    decltype(&waveOutOpen) open=nullptr;decltype(&waveOutWrite) write=nullptr;
    decltype(&waveOutGetPosition) position=nullptr;
    DRVCALLBACK *callback=nullptr;DWORD_PTR userdata=0;HWAVEOUT device=nullptr;
    WAVEFORMATEX format{};
};
static WaveTrace &wave=*new WaveTrace;
static void wave_event(unsigned kind,unsigned bytes,int pending) {
    int64_t at=tick(),begin=trace.begin.load(std::memory_order_relaxed),finish=trace.finish.load(std::memory_order_relaxed);
    if(begin&&at>=begin&&(!finish||at<finish)){
        unsigned index=wave.used.fetch_add(1,std::memory_order_relaxed);
        if(index<32768)wave.rows[index]={at,kind,bytes,pending};else ++wave.dropped;
    }
}
static void CALLBACK tapped_wave_callback(HDRVR device,UINT message,DWORD_PTR instance,DWORD_PTR arg1,DWORD_PTR arg2) {
    (void)instance;
    if(message==WOM_DONE){auto header=(WAVEHDR*)arg1;int pending=wave.pending.fetch_sub(1)-1;wave_event(2,header?header->dwBufferLength:0,pending);}
    wave.callback(device,message,wave.userdata,arg1,arg2);
}
static MMRESULT WINAPI tapped_wave_open(LPHWAVEOUT device,UINT id,LPCWAVEFORMATEX format,DWORD_PTR callback,DWORD_PTR instance,DWORD flags) {
    // SDL tests candidate formats with WAVE_FORMAT_QUERY and no callback.
    if(flags&WAVE_FORMAT_QUERY)return wave.open(device,id,format,callback,instance,flags);
    if((flags&CALLBACK_TYPEMASK)!=CALLBACK_FUNCTION||!callback||!format||wave.opens.fetch_add(1)!=0){++wave.errors;return MMSYSERR_NOTSUPPORTED;}
    wave.callback=(DRVCALLBACK*)callback;wave.userdata=instance;wave.format=*format;
    MMRESULT result=wave.open(device,id,format,(DWORD_PTR)tapped_wave_callback,0,flags);
    if(!result)wave.device=*device;else ++wave.errors;return result;
}
static MMRESULT WINAPI tapped_wave_write(HWAVEOUT device,LPWAVEHDR header,UINT size) {
    int pending=wave.pending.fetch_add(1)+1;wave_event(1,header->dwBufferLength,pending);
    MMRESULT result=wave.write(device,header,size);
    if(result){--wave.pending;++wave.errors;}return result;
}
struct WaveTap {
    struct Entry {void **slot,*original;};std::vector<Entry> entries;
    explicit WaveTap(HMODULE sdl,bool enabled) {
        if(!enabled)return;
        HMODULE winmm=GetModuleHandleW(L"winmm.dll");require(winmm!=nullptr,"SDL WinMM provider not loaded");
        wave.open=load<decltype(wave.open)>(winmm,"waveOutOpen");wave.write=load<decltype(wave.write)>(winmm,"waveOutWrite");
        wave.position=load<decltype(wave.position)>(winmm,"waveOutGetPosition");
        auto *base=(BYTE*)sdl;auto *dos=(IMAGE_DOS_HEADER*)base;auto *nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
        auto *table=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for(;table->Name;table++)if(!_stricmp((char*)base+table->Name,"winmm.dll")){
            auto *names=(IMAGE_THUNK_DATA64*)(base+table->OriginalFirstThunk),*bound=(IMAGE_THUNK_DATA64*)(base+table->FirstThunk);
            for(;names->u1.AddressOfData;names++,bound++)if(!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)){
                auto *item=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);void *replacement=nullptr;
                if(!strcmp((char*)item->Name,"waveOutOpen"))replacement=(void*)tapped_wave_open;
                if(!strcmp((char*)item->Name,"waveOutWrite"))replacement=(void*)tapped_wave_write;
                if(!replacement)continue;
                auto slot=(void**)&bound->u1.Function;void *original=*slot;
                require(original==(void*)GetProcAddress(winmm,(char*)item->Name),"SDL WinMM IAT target mismatch");
                DWORD old;require(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old),"Cannot instrument owned SDL IAT");
                InterlockedExchangePointer(slot,replacement);DWORD ignored;VirtualProtect(slot,sizeof(void*),old,&ignored);
                entries.push_back({slot,original});
            }
        }
        require(entries.size()==2,"SDL WinMM imports missing");
    }
    ~WaveTap(){for(auto &entry:entries){DWORD old;if(VirtualProtect(entry.slot,sizeof(void*),PAGE_READWRITE,&old)){InterlockedExchangePointer(entry.slot,entry.original);DWORD ignored;VirtualProtect(entry.slot,sizeof(void*),old,&ignored);}}}
};
static void __cdecl tapped_callback(void*,uint8_t *buffer,int length) {
    int64_t start=tick();bool injected=false;
    auto after=trace.inject_after.load(std::memory_order_relaxed);
    if(after&&start>=after&&!trace.injected.exchange(true)){Sleep(trace.stall_ms);injected=true;}
    trace.callback(trace.userdata,buffer,length);
    // Even the real title cue is never emitted. This is an owned render buffer,
    // not captured device audio; no PCM is written to diagnostic artifacts.
    if(length>0)memset(buffer,0,(size_t)length);
    int64_t end=tick(),begin=trace.begin.load(std::memory_order_relaxed),finish=trace.finish.load(std::memory_order_relaxed);
    if(begin&&start>=begin&&(!finish||start<finish)){
        unsigned index=trace.used.fetch_add(1,std::memory_order_relaxed);
        if(index<32768)trace.rows[index]={start,end,(uint32_t)std::max(0,length),GetCurrentThreadId(),injected};
        else ++trace.dropped;
    }
}
static uint32_t __cdecl tapped_open(const char *device,int capture,const SdlSpec *want,SdlSpec *have,int changes) {
    if(capture){++trace.capture_attempts;return 0;}
    if(!want||!want->callback||trace.opens.fetch_add(1)!=0)return 0;
    trace.requested=*want;trace.callback=want->callback;trace.userdata=want->userdata;
    SdlSpec copy=*want;copy.callback=tapped_callback;copy.userdata=nullptr;
    SdlSpec actual{};uint32_t id=trace.open(device,0,&copy,&actual,changes);
    if(id){trace.obtained=actual;if(have)*have=actual;}
    return id;
}
struct ImportTap {
    void **slot=nullptr,*original=nullptr;
    explicit ImportTap(HMODULE faudio,HMODULE sdl) {
        auto *base=(BYTE*)faudio;auto *dos=(IMAGE_DOS_HEADER*)base;auto *nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
        require(dos->e_magic==IMAGE_DOS_SIGNATURE&&nt->Signature==IMAGE_NT_SIGNATURE,"Invalid FAudio image");
        auto *table=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for(;table->Name;table++)if(!_stricmp((char*)base+table->Name,"SDL2.dll")){
            require(table->OriginalFirstThunk!=0,"FAudio import names unavailable");
            auto *names=(IMAGE_THUNK_DATA64*)(base+table->OriginalFirstThunk),*bound=(IMAGE_THUNK_DATA64*)(base+table->FirstThunk);
            for(;names->u1.AddressOfData;names++,bound++)if(!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)){
                auto *item=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
                if(!strcmp((char*)item->Name,"SDL_OpenAudioDevice")){slot=(void**)&bound->u1.Function;original=*slot;}
            }
        }
        require(slot&&original==(void*)GetProcAddress(sdl,"SDL_OpenAudioDevice"),"FAudio does not import this copied SDL audio opener");
        trace.open=(SdlOpen)original;DWORD old=0;
        require(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old),"Cannot instrument private process IAT");
        InterlockedExchangePointer(slot,(void*)tapped_open);DWORD ignored;VirtualProtect(slot,sizeof(void*),old,&ignored);
    }
    ~ImportTap(){if(slot){DWORD old;if(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)){InterlockedExchangePointer(slot,original);DWORD ignored;VirtualProtect(slot,sizeof(void*),old,&ignored);}}}
};
struct Counters {
    FAudioEngineCallback engine{};FAudioVoiceCallback voice{};
    std::atomic<unsigned> engine_passes{0},source_passes{0},required_passes{0},engine_errors{0},voice_errors{0};
    std::atomic<uint64_t> required_bytes{0};
    Counters(){
        engine.OnProcessingPassStart=[](FAudioEngineCallback *p){++((Counters*)p)->engine_passes;};
        engine.OnCriticalError=[](FAudioEngineCallback *p,uint32_t){++((Counters*)p)->engine_errors;};
        voice.OnVoiceProcessingPassStart=[](FAudioVoiceCallback *p,uint32_t bytes){auto *c=(Counters*)((BYTE*)p - offsetof(Counters,voice));++c->source_passes;if(bytes){++c->required_passes;c->required_bytes+=bytes;}};
        voice.OnVoiceError=[](FAudioVoiceCallback *p,void*,uint32_t){auto *c=(Counters*)((BYTE*)p - offsetof(Counters,voice));++c->voice_errors;};
    }
};
static std::vector<uint8_t> file(const wchar_t *path) {
    FILE *f=_wfopen(path,L"rb");require(f!=nullptr,"Cannot open copied XACT bank");_fseeki64(f,0,SEEK_END);auto n=_ftelli64(f);rewind(f);
    if(n<=0||n>256*1024*1024){fclose(f);throw std::runtime_error("Invalid copied XACT bank size");}
    std::vector<uint8_t> data((size_t)n);bool ok=fread(data.data(),1,data.size(),f)==data.size();fclose(f);require(ok,"Cannot read complete copied XACT bank");return data;
}
static double percentile(std::vector<double> values,double fraction){if(values.empty())return 0;std::sort(values.begin(),values.end());return values[(size_t)std::ceil(fraction*(values.size()-1))];}
static int run(int argc,wchar_t **argv) {
    require(argc>=6,"usage: MODE(pcm/xact) SECONDS OUTPUT_STEM SDL_DRIVER EXPECTED_DS(-/native/path) [STALL_MS] [MASTER_RATE] [QUANTUM_1024] [WORKLOAD(idle/render)] [WASAPI_TELEMETRY]");
    bool xact=!wcscmp(argv[1],L"xact");require(xact||!wcscmp(argv[1],L"pcm"),"Unknown audio mode");
    unsigned seconds=(unsigned)_wtoi(argv[2]);require(seconds>=5&&seconds<=120,"Duration must be 5..120 seconds");
    trace.stall_ms=argc>6?(unsigned)_wtoi(argv[6]):0;require(trace.stall_ms<=250,"Controlled stall exceeds 250ms");
    unsigned master_rate=argc>7?(unsigned)_wtoi(argv[7]):0;
    require(master_rate==0||master_rate==44100||master_rate==48000,"Unsupported diagnostic master rate");
    bool quantum_1024=argc>8&&_wtoi(argv[8])!=0;
    std::wstring workload=argc>9?argv[9]:L"idle";require(workload==L"idle"||workload==L"render","Unsupported frame workload");
    bool observe_wasapi=(!wcscmp(argv[4],L"wasapi"))&&(argc<=10||_wtoi(argv[10])!=0);
    std::wstring stem=argv[3],expected_driver=argv[4],expected_ds=argv[5];
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);require(SUCCEEDED(com),"COM initialization failed");
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    HMODULE faudio=LoadLibraryW(L"FAudio.dll");require(faudio!=nullptr,"Copied FAudio.dll failed to load");
    HMODULE sdl=GetModuleHandleW(L"SDL2.dll");require(sdl!=nullptr,"Copied SDL2.dll not loaded");
    auto location=module_path(GetModuleHandleW(nullptr));location.resize(location.find_last_of(L"\\/"));
    require(!_wcsicmp(module_path(faudio).c_str(),(location+L"\\FAudio.dll").c_str()),"FAudio did not load from fixture directory");
    require(!_wcsicmp(module_path(sdl).c_str(),(location+L"\\SDL2.dll").c_str()),"SDL did not load from fixture directory");
    Api api(faudio);require(api.FAudioLinkedVersion()==220700,"Expected the game's FAudio 22.7.0 DLL");
    auto get_version=load<void(__cdecl*)(SdlVersion*)>(sdl,"SDL_GetVersion");SdlVersion version{};get_version(&version);
    require(version.major==2&&version.minor==24&&version.patch==0,"Expected the game's SDL 2.24.0 DLL");
    auto get_driver=load<const char*(__cdecl*)()>(sdl,"SDL_GetCurrentAudioDriver");
    auto get_error=load<const char*(__cdecl*)()>(sdl,"SDL_GetError");
    ImportTap import(faudio,sdl);WaveTap wave_import(sdl,expected_driver==L"winmm");game_audio::WasapiTap wasapi_import(sdl,observe_wasapi);
    game_audio::RenderWorkload renderer(workload==L"render");Counters counters;
    FAudio *audio=nullptr;success(api.FAudioCreate(&audio,quantum_1024?FAUDIO_1024_QUANTUM:0,FAUDIO_DEFAULT_PROCESSOR),"FAudioCreate");
    success(api.FAudio_RegisterForCallbacks(audio,&counters.engine),"Register engine callbacks");
    require(get_driver()&&get_driver()==utf8(expected_driver),"SDL did not select requested audio driver");
    FAudioMasteringVoice *master=nullptr;
    uint32_t created=api.FAudio_CreateMasteringVoice(audio,&master,0,master_rate,0,0,nullptr);
    if(created||!master){fprintf(stderr,"SDL error: %s\n",get_error());throw std::runtime_error("No render endpoint opened; timing unavailable");}
    success(api.FAudioVoice_SetVolume(master,0,FAUDIO_COMMIT_NOW),"Mute diagnostic master");float volume=1;api.FAudioVoice_GetVolume(master,&volume);require(volume==0,"Master mute verification failed");
    FAudioVoiceDetails details{};api.FAudioVoice_GetVoiceDetails(master,&details);
    require(trace.opens==1&&trace.capture_attempts==0&&trace.obtained.channels&&trace.obtained.freq>0,"Invalid render callback instrumentation");
    std::wstring ds_path;HMODULE ds=GetModuleHandleW(L"dsound.dll");if(ds)ds_path=module_path(ds);
    if(expected_driver==L"directsound"){
        require(ds!=nullptr,"DirectSound provider not loaded");
        if(expected_ds==L"native"){
            std::vector<wchar_t> sys(32768);UINT n=GetSystemDirectoryW(sys.data(),(UINT)sys.size());require(n&&ds_path.size()>n&&!_wcsnicmp(ds_path.c_str(),sys.data(),n),"Expected native system DirectSound");
        }else require(!_wcsicmp(ds_path.c_str(),expected_ds.c_str()),"DirectSound provider mismatch");
    }
    uint32_t numerator=0,denominator=0;api.FAudio_GetProcessingQuantum(audio,&numerator,&denominator);require(numerator&&denominator,"No engine quantum");
    FAudioSourceVoice *source=nullptr;FACTAudioEngine *fact=nullptr;FACTSoundBank *bank=nullptr;FACTWaveBank *waves=nullptr;FACTCue *cue=nullptr;
    std::vector<uint8_t> globals,soundbank,wavebank;std::vector<int16_t> silence;
    if(xact){
        globals=file(L"TheSounds.xgs");soundbank=file(L"Sound Bank.xsb");wavebank=file(L"Wave Bank.xwb");
        success(api.FACTCreateEngine(0,&fact),"Create XACT engine");
        FACTRuntimeParameters config{};config.lookAheadTime=250;config.pGlobalSettingsBuffer=globals.data();config.globalSettingsBufferSize=(uint32_t)globals.size();config.pXAudio2=audio;config.pMasteringVoice=master;
        success(api.FACTAudioEngine_Initialize(fact,&config),"Initialize actual XACT settings");
        success(api.FACTAudioEngine_CreateInMemoryWaveBank(fact,wavebank.data(),(uint32_t)wavebank.size(),0,0,&waves),"Load actual in-memory wave bank");
        success(api.FACTAudioEngine_CreateSoundBank(fact,soundbank.data(),(uint32_t)soundbank.size(),0,0,&bank),"Load actual sound bank");
        uint16_t index=api.FACTSoundBank_GetCueIndex(bank,"BT2_Title");require(index!=0xffff,"BT2_Title cue not found");
        success(api.FACTSoundBank_Play(bank,index,0,0,&cue),"Start actual title cue under mute");require(cue!=nullptr,"No title cue object");
    }else{
        FAudioWaveFormatEx format{};format.wFormatTag=FAUDIO_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=44100;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=176400;
        success(api.FAudio_CreateSourceVoice(audio,&source,&format,0,2,&counters.voice,nullptr,nullptr),"Create silent source");
        silence.resize(44100*2,0);FAudioBuffer buffer{};buffer.AudioBytes=(uint32_t)(silence.size()*2);buffer.pAudioData=(const uint8_t*)silence.data();buffer.LoopCount=FAUDIO_LOOP_INFINITE;
        success(api.FAudioSourceVoice_SubmitSourceBuffer(source,&buffer,nullptr),"Queue looped silence");success(api.FAudioSourceVoice_Start(source,0,FAUDIO_COMMIT_NOW),"Start silent source");
    }
    success(api.FAudio_StartEngine(audio),"Start mixer");
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);double scale=1000.0/frequency.QuadPart;
    int64_t started=tick(),begin=started+2*frequency.QuadPart,finish=begin+seconds*frequency.QuadPart;
    trace.begin=begin;trace.finish=finish;if(trace.stall_ms)trace.inject_after=begin+frequency.QuadPart;
    game_audio::wasapi_measurement_window(begin,finish,frequency.QuadPart);
    unsigned polls=0,empty_polls=0,active_polls=0,cue_playing=0;uint64_t first_samples=0,last_samples=0;bool sample_started=false;
    DWORD first_wave_samples=0,last_wave_samples=0;unsigned wave_position_polls=0;
    std::vector<double> work_times;work_times.reserve(seconds*60);long long next_frame=started,frame_ticks=frequency.QuadPart/60;
    printf("Running %s driver=%s mode=%s quantum=%u/%u (%g ms), master=%u channels @ %uHz\n",xact?"BT2_Title":"silent PCM",get_driver(),xact?"in-memory XACT bank":"looped PCM",numerator,denominator,1000.0*numerator/denominator,details.InputChannels,details.InputSampleRate);
    while(tick()<finish){
        if(tick()<next_frame){Sleep(1);continue;}
        if(fact){int64_t at=tick();success(api.FACTAudioEngine_DoWork(fact),"XACT DoWork");work_times.push_back((tick()-at)*scale);}
        renderer.frame(begin,finish,frequency.QuadPart);
        if(tick()>=begin){
            ++polls;FAudioPerformanceData perf{};api.FAudio_GetPerformanceData(audio,&perf);if(perf.ActiveSourceVoiceCount)++active_polls;
            if(source){FAudioVoiceState state{};api.FAudioSourceVoice_GetState(source,&state,0);if(!state.BuffersQueued)++empty_polls;if(!sample_started){first_samples=state.SamplesPlayed;sample_started=true;}last_samples=state.SamplesPlayed;}
            if(cue){uint32_t state=0;success(api.FACTCue_GetState(cue,&state),"XACT cue state");if(state&FACT_STATE_PLAYING)++cue_playing;}
            if(wave.device&&!wave_position_polls){MMTIME position{};position.wType=TIME_SAMPLES;if(!wave.position(wave.device,&position,sizeof(position))&&position.wType==TIME_SAMPLES){first_wave_samples=last_wave_samples=position.u.sample;++wave_position_polls;}}
        }
        next_frame+=frame_ticks;if(next_frame<tick()-frame_ticks)next_frame=tick()+frame_ticks;
    }
    puts("Measurement loop completed; stopping mixer");
    // Read the device counter only at the interval boundaries. Continuous
    // waveOutGetPosition polling can itself perturb legacy WinMM scheduling.
    if(wave.device){MMTIME position{};position.wType=TIME_SAMPLES;if(!wave.position(wave.device,&position,sizeof(position))&&position.wType==TIME_SAMPLES){last_wave_samples=position.u.sample;++wave_position_polls;}}
    api.FAudio_StopEngine(audio);api.FAudio_UnregisterForCallbacks(audio,&counters.engine);
    // Match FNA 22.08 AudioEngine.Dispose: stop the API thread before Release.
    // FAudio 22.07 Release holds apiLock, so waiting for that thread from inside
    // Release can deadlock while it is trying to acquire the same lock.
    if(fact){puts("Shutting down XACT engine");api.FACTAudioEngine_ShutDown(fact);api.FACTAudioEngine_Release(fact);fact=nullptr;audio=nullptr;master=nullptr;}
    else{api.FAudioVoice_DestroyVoice(source);api.FAudioVoice_DestroyVoice(master);api.FAudio_Release(audio);}
    puts("Audio resources released; writing timing report");
    unsigned count=std::min(32768u,trace.used.load());require(count>20&&trace.dropped==0,"Insufficient callback trace");
    double quantum=1000.0*numerator/denominator;std::vector<double> gaps,costs;uint64_t total_bytes=0;unsigned gap2=0,gap4=0,overruns=0;
    FILE *csv=_wfopen((stem+L".csv").c_str(),L"wb");require(csv!=nullptr,"Cannot write callback trace");fputs("index,start_ms,gap_ms,callback_ms,bytes,thread_id,injected_stall\n",csv);
    for(unsigned i=0;i<count;i++){
        auto &row=trace.rows[i];double cost=(row.end-row.start)*scale,gap=i?(row.start-trace.rows[i-1].start)*scale:0;costs.push_back(cost);total_bytes+=row.length;
        if(i){gaps.push_back(gap);if(gap>2*quantum)++gap2;if(gap>4*quantum)++gap4;}if(cost>quantum)++overruns;
        fprintf(csv,"%u,%.6f,%.6f,%.6f,%u,%u,%u\n",i,(row.start-begin)*scale,gap,cost,row.length,row.thread,row.injected?1:0);
    }fclose(csv);
    unsigned wave_count=std::min(32768u,wave.used.load()),wave_writes=0,wave_done=0,wave_zero=0;uint64_t wave_write_bytes=0,wave_done_bytes=0;std::vector<double> done_gaps;int64_t previous_done=0;
    if(wave_count){
        FILE *events=_wfopen((stem+L"-wave.csv").c_str(),L"wb");require(events!=nullptr,"Cannot write WinMM timing metadata");
        fputs("index,start_ms,event,bytes,outstanding_submissions\n",events);
        for(unsigned i=0;i<wave_count;i++){auto &row=wave.rows[i];
            if(row.kind==1){++wave_writes;wave_write_bytes+=row.bytes;}
            else{++wave_done;wave_done_bytes+=row.bytes;if(!row.pending)++wave_zero;if(previous_done)done_gaps.push_back((row.at-previous_done)*scale);previous_done=row.at;}
            fprintf(events,"%u,%.6f,%s,%u,%d\n",i,(row.at-begin)*scale,row.kind==1?"write":"done",row.bytes,row.pending);
        }fclose(events);
    }
    unsigned bytes_per_frame=(trace.obtained.format&0xff)/8*trace.obtained.channels;require(bytes_per_frame>0,"Invalid callback frame format");
    double delivered_seconds=(double)total_bytes/bytes_per_frame/trace.obtained.freq;
    bool passed=counters.engine_errors==0&&counters.voice_errors==0&&wave.errors==0&&wave.dropped==0&&trace.capture_attempts==0&&active_polls==polls&&polls>0&&(!source||(!empty_polls&&last_samples>first_samples))&&(!xact||cue_playing==polls);
    if(trace.stall_ms)passed=passed&&trace.injected&&*std::max_element(costs.begin(),costs.end())>=trace.stall_ms*0.8;
    double rate_ratio=delivered_seconds/seconds;bool normal_progress=rate_ratio>=.9&&rate_ratio<=1.1;
    const char *classification=trace.stall_ms?"controlled-callback-stall":!normal_progress?"callback-frame-progress-anomaly":(gap4||overruns)?"long-callback-delay-observed":"no-large-callback-delay-observed";
    FILE *report=_wfopen((stem+L".json").c_str(),L"wb");require(report!=nullptr,"Cannot write timing report");
    fputs("{\n",report);auto wasapi=game_audio::wasapi_report(stem,report,observe_wasapi);renderer.report(stem,report);
    if(observe_wasapi)passed=passed&&wasapi.complete;
    if(observe_wasapi&&trace.stall_ms)passed=passed&&wasapi.max_submit_gap_ms>=trace.stall_ms*.8;
    fprintf(report,"\"collectionSucceeded\":%s,\"classification\":%s,\"callbackFrameRateRatio\":%.6f,\"progressWithinTolerance\":%s,\"mode\":%s,\"driver\":%s,\"faudioVersion\":220700,\"sdlVersion\":\"2.24.0\",\n",passed?"true":"false",json(classification).c_str(),rate_ratio,normal_progress?"true":"false",json(xact?"xact-in-memory-BT2_Title":"pcm-silent-loop").c_str(),json(get_driver()?get_driver():utf8(expected_driver)).c_str());
    fprintf(report,"\"faudioPath\":%s,\"sdlPath\":%s,\"dsoundPath\":%s,\n",json(utf8(module_path(faudio))).c_str(),json(utf8(module_path(sdl))).c_str(),json(utf8(ds_path)).c_str());
    fprintf(report,"\"measuredSeconds\":%u,\"warmupSeconds\":2,\"quantumMs\":%.6f,\"callbackRateHz\":%d,\"callbackChannels\":%u,\"callbackFormat\":%u,\"requestedFrames\":%u,\"obtainedFrames\":%u,\n",seconds,quantum,trace.obtained.freq,trace.obtained.channels,trace.obtained.format,trace.requested.samples,trace.obtained.samples);
    fprintf(report,"\"requestedMasterRate\":%u,\"quantum1024\":%s,\n",master_rate,quantum_1024?"true":"false");
    fprintf(report,"\"waveOut\":{\"opens\":%u,\"rate\":%lu,\"channels\":%u,\"formatTag\":%u,\"bits\":%u,\"writes\":%u,\"completions\":%u,\"submittedBytes\":%llu,\"completedBytes\":%llu,\"zeroOutstandingNotifications\":%u,\"completionGapP99Ms\":%.6f,\"completionGapMaxMs\":%.6f,\"positionPolls\":%u,\"positionSamplesAdvanced\":%lu,\"errors\":%u},\n",wave.opens.load(),wave.format.nSamplesPerSec,wave.format.nChannels,wave.format.wFormatTag,wave.format.wBitsPerSample,wave_writes,wave_done,(unsigned long long)wave_write_bytes,(unsigned long long)wave_done_bytes,wave_zero,percentile(done_gaps,.99),percentile(done_gaps,1),wave_position_polls,last_wave_samples-first_wave_samples,wave.errors.load());
    fprintf(report,"\"callbacks\":%u,\"callbackAudioSeconds\":%.6f,\"gapP50Ms\":%.6f,\"gapP95Ms\":%.6f,\"gapP99Ms\":%.6f,\"gapMaxMs\":%.6f,\"gapsOver2xQuantum\":%u,\"gapsOver4xQuantum\":%u,\n",count,delivered_seconds,percentile(gaps,.5),percentile(gaps,.95),percentile(gaps,.99),percentile(gaps,1),gap2,gap4);
    fprintf(report,"\"callbackP95Ms\":%.6f,\"callbackP99Ms\":%.6f,\"callbackMaxMs\":%.6f,\"callbacksOverQuantum\":%u,\"xactDoWorkP99Ms\":%.6f,\"xactDoWorkMaxMs\":%.6f,\n",percentile(costs,.95),percentile(costs,.99),percentile(costs,1),overruns,percentile(work_times,.99),percentile(work_times,1));
    fprintf(report,"\"engineErrors\":%u,\"voiceErrors\":%u,\"sourceEmptyPolls\":%u,\"sourceRequestedRefillPasses\":%u,\"sourceRequestedRefillBytes\":%llu,\"sourceSamplesAdvanced\":%llu,\"polls\":%u,\"activeSourcePolls\":%u,\"cuePlayingPolls\":%u,\n",counters.engine_errors.load(),counters.voice_errors.load(),empty_polls,counters.required_passes.load(),(unsigned long long)counters.required_bytes.load(),(unsigned long long)(last_samples-first_samples),polls,active_polls,cue_playing);
    fprintf(report,"\"injectedStallMs\":%u,\"capturePerformed\":false,\"outputForcedSilent\":true,\"hardwareUnderruns\":null,\"limitations\":[\"Callback scheduling gaps are not hardware underrun counts\",\"FAudio glitch counter is unimplemented and not used\",\"XACT bank is loaded in memory; original game construction is not assumed\",\"FAudio source queue checks apply only to the synthetic PCM voice\",\"Silent isolated fixture does not establish game audibility or whole-game stability\"]\n}\n",trace.stall_ms);fclose(report);
    printf("%s classification=%s frameRateRatio=%.3f callbacks=%u maxGap=%.3fms maxCallback=%.3fms gap>2q=%u activePolls=%u/%u\n",passed?"COLLECTED":"FAILED",classification,rate_ratio,count,percentile(gaps,1),percentile(costs,1),gap2,active_polls,polls);
    if(observe_wasapi)printf("WASAPI observed=%u submissions=%u zeroPadding=%u clockSamples=%u maxSubmitGap=%.3fms clockObserverMax=%.3fms\n",wasapi.complete?1:0,wasapi.submissions,wasapi.empty_padding,wasapi.clock_samples,wasapi.max_submit_gap_ms,wasapi.max_clock_observer_ms);
    return passed?0:1;
}
int wmain(int argc,wchar_t **argv){setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);try{return run(argc,argv);}catch(const std::exception &e){fprintf(stderr,"Audio timing unavailable: %s\n",e.what());fflush(stderr);TerminateProcess(GetCurrentProcess(),2);return 2;}}
