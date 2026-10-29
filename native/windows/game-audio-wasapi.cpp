// SPDX-License-Identifier: LGPL-2.1-or-later
// Observe the real interfaces used by SDL 2.24's WASAPI backend. No capture,
// padding polling loop, altered stream flags, or remote process operations.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "game-audio-wasapi.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace game_audio { namespace {
enum Kind {get_buffer=1,release_buffer=2,padding=3,clock_position=4,start_client=5,stop_client=6};
struct Row {long long at=0,cost=0;unsigned kind=0,client=0,frames=0,flags=0;HRESULT status=0;UINT64 position=0,qpc100ns=0;};
struct ClientRecord {
    unsigned id=0,rate=0,channels=0,bits=0,format=0,buffer_frames=0,stream_flags=0;
    AUDCLNT_SHAREMODE mode=AUDCLNT_SHAREMODE_SHARED;
    REFERENCE_TIME default_period=0,min_period=0,requested_duration=0,requested_period=0;
    bool default_period_observed=false,min_period_observed=false;
    HRESULT initialize_result=E_PENDING,clock_result=E_PENDING,frequency_result=E_PENDING;
    UINT64 frequency=0;
    IAudioClock *clock=nullptr;
    std::atomic<long long> last_clock{0};
    std::atomic<unsigned> render_services{0},unexpected_client_qi{0};
};
struct Trace {
    static constexpr unsigned capacity=131072;
    Row rows[capacity]{};
    std::atomic<unsigned> used{0},dropped{0},enumerators{0},capture_requests{0};
    std::atomic<long long> begin{0},finish{0},frequency{0};
    std::mutex lock;
    std::vector<ClientRecord*> clients;
    decltype(&CoCreateInstance) create=nullptr;
};
static Trace &trace=*new Trace;
static long long tick(){LARGE_INTEGER value;QueryPerformanceCounter(&value);return value.QuadPart;}
static void require(bool value,const char *what){if(!value)throw std::runtime_error(what);}
static void record(unsigned kind,ClientRecord *client,long long at,long long cost,HRESULT status,unsigned frames=0,unsigned flags=0,UINT64 position=0,UINT64 qpc=0){
    auto begin=trace.begin.load(std::memory_order_relaxed),end=trace.finish.load(std::memory_order_relaxed);
    if(!begin||at<begin||at>=end)return;
    unsigned index=trace.used.fetch_add(1,std::memory_order_relaxed);
    if(index<Trace::capacity)trace.rows[index]={at,cost,kind,client->id,frames,flags,status,position,qpc};
    else ++trace.dropped;
}
static void sample_clock(ClientRecord *client){
    long long frequency=trace.frequency.load(),now=tick(),last=client->last_clock.load();
    if(!client->clock||!frequency||now-last<frequency/10||!client->last_clock.compare_exchange_strong(last,now))return;
    UINT64 position=0,qpc=0;HRESULT result=client->clock->GetPosition(&position,&qpc);
    record(clock_position,client,now,tick()-now,result,0,0,position,qpc);
}
class RenderProxy final:public IAudioRenderClient {
    std::atomic<ULONG> references{1};IAudioRenderClient *inner;ClientRecord *state;
public:
    RenderProxy(IAudioRenderClient *p,ClientRecord *s):inner(p),state(s){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioRenderClient)){*out=static_cast<IAudioRenderClient*>(this);AddRef();return S_OK;}
        return inner->QueryInterface(iid,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG count=--references;if(!count){inner->Release();delete this;}return count;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE **data) override {
        long long at=tick();HRESULT result=inner->GetBuffer(frames,data);record(get_buffer,state,at,tick()-at,result,frames);return result;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override {
        long long at=tick();HRESULT result=inner->ReleaseBuffer(frames,flags);record(release_buffer,state,at,tick()-at,result,frames,flags);sample_clock(state);return result;
    }
};
class ClientProxy final:public IAudioClient {
    std::atomic<ULONG> references{1};IAudioClient *inner;ClientRecord *state;
public:
    explicit ClientProxy(IAudioClient *p):inner(p),state(new ClientRecord){std::lock_guard<std::mutex> guard(trace.lock);state->id=unsigned(trace.clients.size()+1);trace.clients.push_back(state);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioClient)){*out=static_cast<IAudioClient*>(this);AddRef();return S_OK;}
        ++state->unexpected_client_qi;return inner->QueryInterface(iid,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {ULONG count=--references;if(!count){if(state->clock){state->clock->Release();state->clock=nullptr;}inner->Release();delete this;}return count;}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME duration,REFERENCE_TIME period,const WAVEFORMATEX *format,LPCGUID session) override {
        state->mode=mode;state->stream_flags=flags;state->requested_duration=duration;state->requested_period=period;
        if(format){state->rate=format->nSamplesPerSec;state->channels=format->nChannels;state->bits=format->wBitsPerSample;state->format=format->wFormatTag;}
        HRESULT result=inner->Initialize(mode,flags,duration,period,format,session);state->initialize_result=result;
        if(SUCCEEDED(result)){
            state->clock_result=inner->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&state->clock));
            if(SUCCEEDED(state->clock_result))state->frequency_result=state->clock->GetFrequency(&state->frequency);
        }return result;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32 *frames) override {HRESULT result=inner->GetBufferSize(frames);if(SUCCEEDED(result)&&frames)state->buffer_frames=*frames;return result;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME *time) override{return inner->GetStreamLatency(time);}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32 *frames) override {
        long long at=tick();HRESULT result=inner->GetCurrentPadding(frames);record(padding,state,at,tick()-at,result,SUCCEEDED(result)&&frames?*frames:0);return result;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode,const WAVEFORMATEX *format,WAVEFORMATEX **closest) override{return inner->IsFormatSupported(mode,format,closest);}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX **format) override{return inner->GetMixFormat(format);}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME *normal,REFERENCE_TIME *minimum) override {HRESULT result=inner->GetDevicePeriod(normal,minimum);if(SUCCEEDED(result)){if(normal){state->default_period=*normal;state->default_period_observed=true;}if(minimum){state->min_period=*minimum;state->min_period_observed=true;}}return result;}
    HRESULT STDMETHODCALLTYPE Start() override {long long at=tick();HRESULT result=inner->Start();record(start_client,state,at,tick()-at,result);return result;}
    HRESULT STDMETHODCALLTYPE Stop() override {long long at=tick();HRESULT result=inner->Stop();record(stop_client,state,at,tick()-at,result);return result;}
    HRESULT STDMETHODCALLTYPE Reset() override{return inner->Reset();}
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override{return inner->SetEventHandle(event);}
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid,void **out) override {
        HRESULT result=inner->GetService(iid,out);if(FAILED(result)||!out||!*out)return result;
        if(iid==__uuidof(IAudioRenderClient)){++state->render_services;*out=static_cast<IAudioRenderClient*>(new RenderProxy(static_cast<IAudioRenderClient*>(*out),state));}
        else if(iid==__uuidof(IAudioCaptureClient))++trace.capture_requests;
        return result;
    }
};
class DeviceProxy final:public IMMDevice {
    std::atomic<ULONG> references{1};IMMDevice *inner;
public:
    explicit DeviceProxy(IMMDevice *p):inner(p){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IMMDevice)){*out=static_cast<IMMDevice*>(this);AddRef();return S_OK;}
        return inner->QueryInterface(iid,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{ULONG count=--references;if(!count){inner->Release();delete this;}return count;}
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid,DWORD context,PROPVARIANT *activation,void **out) override {
        HRESULT result=inner->Activate(iid,context,activation,out);
        if(SUCCEEDED(result)&&out&&*out&&iid==__uuidof(IAudioClient))*out=static_cast<IAudioClient*>(new ClientProxy(static_cast<IAudioClient*>(*out)));
        return result;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access,IPropertyStore **out) override{return inner->OpenPropertyStore(access,out);}
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *out) override{return inner->GetId(out);}
    HRESULT STDMETHODCALLTYPE GetState(DWORD *out) override{return inner->GetState(out);}
};
static HRESULT wrap_device(HRESULT result,IMMDevice **device){if(SUCCEEDED(result)&&device&&*device)*device=new DeviceProxy(*device);return result;}
class CollectionProxy final:public IMMDeviceCollection {
    std::atomic<ULONG> references{1};IMMDeviceCollection *inner;
public:
    explicit CollectionProxy(IMMDeviceCollection *p):inner(p){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IMMDeviceCollection)){*out=static_cast<IMMDeviceCollection*>(this);AddRef();return S_OK;}
        return inner->QueryInterface(iid,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{ULONG count=--references;if(!count){inner->Release();delete this;}return count;}
    HRESULT STDMETHODCALLTYPE GetCount(UINT *count) override{return inner->GetCount(count);}
    HRESULT STDMETHODCALLTYPE Item(UINT index,IMMDevice **out) override{return wrap_device(inner->Item(index,out),out);}
};
class EnumeratorProxy final:public IMMDeviceEnumerator {
    std::atomic<ULONG> references{1};IMMDeviceEnumerator *inner;
public:
    explicit EnumeratorProxy(IMMDeviceEnumerator *p):inner(p){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IMMDeviceEnumerator)){*out=static_cast<IMMDeviceEnumerator*>(this);AddRef();return S_OK;}
        return inner->QueryInterface(iid,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{ULONG count=--references;if(!count){inner->Release();delete this;}return count;}
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow,DWORD mask,IMMDeviceCollection **out) override {HRESULT result=inner->EnumAudioEndpoints(flow,mask,out);if(SUCCEEDED(result)&&out&&*out)*out=new CollectionProxy(*out);return result;}
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow,ERole role,IMMDevice **out) override{return wrap_device(inner->GetDefaultAudioEndpoint(flow,role,out),out);}
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id,IMMDevice **out) override{return wrap_device(inner->GetDevice(id,out),out);}
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *callback) override{return inner->RegisterEndpointNotificationCallback(callback);}
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *callback) override{return inner->UnregisterEndpointNotificationCallback(callback);}
};
static HRESULT WINAPI observed_create(REFCLSID clsid,IUnknown *outer,DWORD context,REFIID iid,void **out){
    HRESULT result=trace.create(clsid,outer,context,iid,out);
    if(SUCCEEDED(result)&&!outer&&clsid==__uuidof(MMDeviceEnumerator)&&iid==__uuidof(IMMDeviceEnumerator)&&out&&*out){++trace.enumerators;*out=static_cast<IMMDeviceEnumerator*>(new EnumeratorProxy(static_cast<IMMDeviceEnumerator*>(*out)));}
    return result;
}
} // namespace
WasapiTap::WasapiTap(HMODULE sdl,bool enabled){
    if(!enabled)return;
    auto *base=reinterpret_cast<BYTE*>(sdl);auto *dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto *nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    require(dos->e_magic==IMAGE_DOS_SIGNATURE&&nt->Signature==IMAGE_NT_SIGNATURE,"Invalid copied SDL image");
    auto *imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;imports++)if(!_stricmp(reinterpret_cast<char*>(base+imports->Name),"ole32.dll")){
        require(imports->OriginalFirstThunk!=0,"SDL import names unavailable");
        auto *names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->OriginalFirstThunk),*bound=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->FirstThunk);
        for(;names->u1.AddressOfData;names++,bound++)if(!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)){
            auto *name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if(!strcmp(reinterpret_cast<char*>(name->Name),"CoCreateInstance")){slot_=reinterpret_cast<void**>(&bound->u1.Function);original_=*slot_;}
        }
    }
    require(slot_&&original_,"Copied SDL does not import CoCreateInstance");trace.create=reinterpret_cast<decltype(trace.create)>(original_);
    DWORD old=0;require(VirtualProtect(slot_,sizeof(void*),PAGE_READWRITE,&old),"Cannot observe copied SDL COM activation");
    InterlockedExchangePointer(slot_,reinterpret_cast<void*>(observed_create));DWORD ignored;VirtualProtect(slot_,sizeof(void*),old,&ignored);
}
WasapiTap::~WasapiTap(){if(slot_){DWORD old;if(VirtualProtect(slot_,sizeof(void*),PAGE_READWRITE,&old)){InterlockedExchangePointer(slot_,original_);DWORD ignored;VirtualProtect(slot_,sizeof(void*),old,&ignored);}}}
void wasapi_measurement_window(long long begin,long long finish,long long frequency){trace.frequency=frequency;trace.finish=finish;trace.begin=begin;}
WasapiSummary wasapi_report(const std::wstring &stem,FILE *json,bool expected){
    WasapiSummary summary;summary.clients=unsigned(trace.clients.size());summary.dropped=trace.dropped;
    unsigned count=std::min(trace.used.load(),Trace::capacity);std::vector<Row> rows(trace.rows,trace.rows+count);
    std::sort(rows.begin(),rows.end(),[](const Row &a,const Row &b){return a.at<b.at;});
    long long frequency=trace.frequency.load(),begin=trace.begin.load();double scale=frequency?1000.0/frequency:0;
    std::vector<long long> previous_submit(summary.clients+1);std::vector<UINT64> first_clock(summary.clients+1),last_clock(summary.clients+1),first_qpc(summary.clients+1),last_qpc(summary.clients+1),submitted(summary.clients+1);
    std::vector<unsigned> clock_counts(summary.clients+1),clock_sfalse(summary.clients+1),clock_backwards(summary.clients+1);
    unsigned min_padding=~0u,max_padding=0;double max_observer=0;
    FILE *csv=nullptr;if(expected){csv=_wfopen((stem+L"-wasapi.csv").c_str(),L"wb");require(csv!=nullptr,"Cannot write WASAPI timing report");fputs("client,event,start_ms,call_ms,hresult,frames,flags,clock_position,clock_qpc_100ns\n",csv);}
    for(auto &row:rows){
        if(FAILED(row.status))++summary.errors;
        max_observer=std::max(max_observer,row.cost*scale);
        if(row.kind==release_buffer&&SUCCEEDED(row.status)){
            ++summary.submissions;submitted[row.client]+=row.frames;
            if(previous_submit[row.client])summary.max_submit_gap_ms=std::max(summary.max_submit_gap_ms,(row.at-previous_submit[row.client])*scale);
            previous_submit[row.client]=row.at;
        }
        if(row.kind==padding&&SUCCEEDED(row.status)){++summary.padding_calls;if(!row.frames)++summary.empty_padding;min_padding=std::min(min_padding,row.frames);max_padding=std::max(max_padding,row.frames);}
        if(row.kind==clock_position&&SUCCEEDED(row.status)){
            ++summary.clock_samples;summary.max_clock_observer_ms=std::max(summary.max_clock_observer_ms,row.cost*scale);
            if(row.status==S_FALSE)++clock_sfalse[row.client];
            if(clock_counts[row.client]){if(row.position<last_clock[row.client]||row.qpc100ns<last_qpc[row.client])++clock_backwards[row.client];}
            else{first_clock[row.client]=row.position;first_qpc[row.client]=row.qpc100ns;}
            last_clock[row.client]=row.position;last_qpc[row.client]=row.qpc100ns;++clock_counts[row.client];
        }
        if(csv)fprintf(csv,"%u,%u,%.6f,%.6f,0x%08lx,%u,%u,%llu,%llu\n",row.client,row.kind,(row.at-begin)*scale,row.cost*scale,ULONG(row.status),row.frames,row.flags,(unsigned long long)row.position,(unsigned long long)row.qpc100ns);
    }if(csv)fclose(csv);
    bool clients_ok=!trace.clients.empty();
    for(auto *client:trace.clients){clients_ok&=SUCCEEDED(client->initialize_result)&&SUCCEEDED(client->clock_result)&&SUCCEEDED(client->frequency_result)&&client->frequency&&client->buffer_frames&&client->rate&&client->render_services==1&&!client->unexpected_client_qi&&clock_counts[client->id]>1&&!clock_backwards[client->id];if(client->rate)summary.submitted_seconds+=double(submitted[client->id])/client->rate;}
    summary.complete=expected&&clients_ok&&summary.clients==1&&trace.enumerators>0&&summary.submissions>20&&summary.padding_calls>20&&summary.clock_samples>2&&!summary.errors&&!summary.dropped&&!trace.capture_requests;
    fprintf(json,"\"wasapi\":{\"expected\":%s,\"complete\":%s,\"clients\":%u,\"submissions\":%u,\"submittedSeconds\":%.6f,\"maxSubmitGapMs\":%.6f,\"paddingCalls\":%u,\"zeroPaddingObservations\":%u,\"minimumPaddingFrames\":%u,\"maximumPaddingFrames\":%u,\"clockSamples\":%u,\"maxClockObserverMs\":%.6f,\"maxObservedApiCallMs\":%.6f,\"errors\":%u,\"droppedEvents\":%u,\"captureRequests\":%u,\"streams\":[",expected?"true":"false",summary.complete?"true":"false",summary.clients,summary.submissions,summary.submitted_seconds,summary.max_submit_gap_ms,summary.padding_calls,summary.empty_padding,min_padding==~0u?0:min_padding,max_padding,summary.clock_samples,summary.max_clock_observer_ms,max_observer,summary.errors,summary.dropped,trace.capture_requests.load());
    for(unsigned i=0;i<trace.clients.size();i++){
        auto *c=trace.clients[i];unsigned id=c->id;double device_seconds=c->frequency?double(last_clock[id]-first_clock[id])/c->frequency:0,qpc_seconds=double(last_qpc[id]-first_qpc[id])/1e7;
        fprintf(json,"%s{\"id\":%u,\"rate\":%u,\"channels\":%u,\"bits\":%u,\"formatTag\":%u,\"shareMode\":%u,\"streamFlags\":%u,\"bufferFrames\":%u,\"defaultPeriodMs\":%.6f,\"minimumPeriodMs\":%s,\"requestedDurationMs\":%.6f,\"requestedPeriodMs\":%.6f,\"initializeHresult\":%ld,\"clockHresult\":%ld,\"frequencyHresult\":%ld,\"clockFrequency\":%llu,\"clockAdvancedSeconds\":%.6f,\"clockCorrelatedQpcSeconds\":%.6f,\"clockRateRatio\":%.6f,\"clockSFalse\":%u,\"clockBackwards\":%u,\"unexpectedClientQueries\":%u}",i?",":"",id,c->rate,c->channels,c->bits,c->format,unsigned(c->mode),c->stream_flags,c->buffer_frames,c->default_period/10000.0,c->min_period_observed?std::to_string(c->min_period/10000.0).c_str():"null",c->requested_duration/10000.0,c->requested_period/10000.0,long(c->initialize_result),long(c->clock_result),long(c->frequency_result),(unsigned long long)c->frequency,device_seconds,qpc_seconds,qpc_seconds?device_seconds/qpc_seconds:0,clock_sfalse[id],clock_backwards[id],c->unexpected_client_qi.load());
    }
    fputs("],\"hardwareUnderruns\":null,\"eventKinds\":{\"1\":\"get-buffer\",\"2\":\"release-buffer\",\"3\":\"padding\",\"4\":\"clock\",\"5\":\"start\",\"6\":\"stop\"},\"limitations\":[\"Zero padding observations are not hardware underrun counts\",\"Clock observations are sampled only during render submissions\",\"Only the owned fixture's copied SDL COM activation is observed\",\"No PCM or loopback capture is performed\"]},\n",json);return summary;
}
} // namespace game_audio
