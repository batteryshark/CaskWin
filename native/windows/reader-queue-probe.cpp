// SPDX-License-Identifier: LGPL-2.1-or-later
// Controls public work-item submission; never inspects Wine's private command layout.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include "vendor/Detours-4.0.1/include/detours.h"
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static constexpr DWORD audio_stream=(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
static void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
static void checked(HRESULT hr,const char *message){if(FAILED(hr)){printf("HRESULT %08lx\n",(unsigned long)hr);throw std::runtime_error(message);}}
static void wait(HANDLE event,const char *message){require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,message);}
static HMODULE owner(IUnknown *object){HMODULE module=nullptr;if(object)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(*(void***)object)[0],&module);return module;}
static std::wstring path(HMODULE module){wchar_t text[32768]{};GetModuleFileNameW(module,text,32768);return text;}
static decltype(&MFPutWorkItem) real_put=MFPutWorkItem;
struct Forward : IMFAsyncCallback {
    LONG refs=1;ComPtr<IMFAsyncCallback> callback;HANDLE complete;
    Forward(IMFAsyncCallback *value,HANDLE signal):callback(value),complete(signal){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMFAsyncCallback))return E_NOINTERFACE;*out=(IMFAsyncCallback*)this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs);}ULONG STDMETHODCALLTYPE Release()override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD *flags,DWORD *queue)override{return callback->GetParameters(flags,queue);}
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult *result)override{HRESULT hr=callback->Invoke(result);SetEvent(complete);return hr;}
};
struct Work {DWORD queue=0;ComPtr<IMFAsyncCallback> callback;ComPtr<IUnknown> state;};
struct Gate {
    SRWLOCK lock=SRWLOCK_INIT;DWORD main=GetCurrentThreadId();HMODULE module=nullptr;IMFAsyncCallback *commands=nullptr;
    HANDLE captured=CreateEventW(nullptr,FALSE,FALSE,nullptr),complete=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    bool capture=false,wrap_main=false;std::deque<Work> held;
    void arm(){AcquireSRWLockExclusive(&lock);capture=true;ReleaseSRWLockExclusive(&lock);}
    void wrap(){ResetEvent(complete);AcquireSRWLockExclusive(&lock);wrap_main=true;ReleaseSRWLockExclusive(&lock);}
    void release(bool signal=false){
        Work work;AcquireSRWLockExclusive(&lock);if(!held.empty()){work=std::move(held.front());held.pop_front();}ReleaseSRWLockExclusive(&lock);require(work.callback!=nullptr,"held work item exists");
        if(signal){ResetEvent(complete);auto proxy=new Forward(work.callback.Get(),complete);HRESULT hr=real_put(work.queue,proxy,work.state.Get());proxy->Release();checked(hr,"release held work through completion proxy");}
        else checked(real_put(work.queue,work.callback.Get(),work.state.Get()),"release held work item");
    }
} gate;
static HRESULT WINAPI gated_put(DWORD queue,IMFAsyncCallback *callback,IUnknown *state){
    if(owner(callback)!=gate.module||owner(state)!=gate.module)return real_put(queue,callback,state);
    bool capture=false,wrap=false;AcquireSRWLockExclusive(&gate.lock);
    bool main=GetCurrentThreadId()==gate.main;
    if(main&&!gate.commands)gate.commands=callback;
    if(callback==gate.commands){
        if(!main&&gate.capture){gate.capture=false;gate.held.push_back({queue,callback,state});capture=true;}
        else if(main&&gate.wrap_main){gate.wrap_main=false;wrap=true;}
    }
    ReleaseSRWLockExclusive(&gate.lock);
    if(capture){puts("GATE captured reader background response work");SetEvent(gate.captured);return S_OK;}
    if(wrap){auto proxy=new Forward(callback,gate.complete);HRESULT hr=real_put(queue,proxy,state);proxy->Release();return hr;}
    return real_put(queue,callback,state);
}
struct Result {HRESULT status;DWORD flags;LONGLONG time;ComPtr<IMFSample> sample;};
struct Callback : IMFSourceReaderCallback {
    LONG refs=1;SRWLOCK lock=SRWLOCK_INIT;std::deque<Result> results;HANDLE ready=CreateSemaphoreW(nullptr,0,256,nullptr),flushed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    IMFSourceReader *reader=nullptr;LONG reentrant=0;HRESULT reentrant_status=S_OK;unsigned total=0;
    ~Callback(){CloseHandle(ready);CloseHandle(flushed);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMFSourceReaderCallback))return E_NOINTERFACE;*out=(IMFSourceReaderCallback*)this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs);}ULONG STDMETHODCALLTYPE Release()override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status,DWORD stream,DWORD flags,LONGLONG time,IMFSample *sample)override{
        printf("CALLBACK stream=%lu status=%08lx flags=%08lx time=%lld sample=%u\n",stream,(unsigned long)status,flags,time,sample!=nullptr);
        if(InterlockedCompareExchange(&reentrant,0,1)==1)reentrant_status=reader->ReadSample(audio_stream,0,nullptr,nullptr,nullptr,nullptr);
        AcquireSRWLockExclusive(&lock);results.push_back({status,flags,time,sample});++total;ReleaseSRWLockExclusive(&lock);ReleaseSemaphore(ready,1,nullptr);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD)override{SetEvent(flushed);return S_OK;}HRESULT STDMETHODCALLTYPE OnEvent(DWORD,IMFMediaEvent*)override{return S_OK;}
    unsigned count(){AcquireSRWLockShared(&lock);unsigned value=total;ReleaseSRWLockShared(&lock);return value;}
    Result take_raw(){wait(ready,"requested read callback completes");AcquireSRWLockExclusive(&lock);auto result=std::move(results.front());results.pop_front();ReleaseSRWLockExclusive(&lock);return result;}
    Result take(){auto result=take_raw();checked(result.status,"callback status succeeds");require(!(result.flags&MF_SOURCE_READERF_ERROR),"callback has no stream error");return result;}
};
static void read(IMFSourceReader *reader,DWORD flags=0){checked(reader->ReadSample(audio_stream,flags,nullptr,nullptr,nullptr,nullptr),"queue asynchronous audio read");}
static void sample(const Result &r){require(r.sample!=nullptr,"callback has actual PCM sample");DWORD bytes=0;checked(r.sample->GetTotalLength(&bytes),"sample buffer length");require(bytes!=0,"sample has PCM bytes");}
int wmain(int argc,wchar_t **argv){
    if(argc!=4){puts("reader-queue-probe <native|absolute-reader.dll> <pcm.wav> <race|drain|flush|reentrant|eos|deselect>");return 2;}
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
    bool hooked=false,started=false,initialized=false,native_error_state=false;int exit_code=1;
    ComPtr<IMFSourceReader> reader;ComPtr<Callback> callback;HMODULE module=nullptr;
    try{
        checked(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"initialize COM");initialized=true;checked(MFStartup(MF_VERSION),"start Media Foundation");started=true;
        bool native=!wcscmp(argv[1],L"native");module=native?LoadLibraryW(L"mfreadwrite.dll"):LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);require(module!=nullptr,"load requested source reader");
        auto create=(decltype(&MFCreateSourceReaderFromURL))GetProcAddress(module,"MFCreateSourceReaderFromURL");require(create!=nullptr,"resolve source reader factory");
        callback.Attach(new Callback);ComPtr<IMFAttributes> attributes;checked(MFCreateAttributes(&attributes,2),"create reader attributes");checked(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get()),"set asynchronous callback");
        checked(create(argv[2],attributes.Get(),&reader),"create real WAV source reader");callback->reader=reader.Get();require(owner(reader.Get())==module,"actual reader object belongs to requested provider");wprintf(L"READER %ls\n",path(module).c_str());
        if(!native){gate.module=module;require(DetourTransactionBegin()==NO_ERROR,"begin work gate");DetourUpdateThread(GetCurrentThread());require(DetourAttach((PVOID*)&real_put,gated_put)==NO_ERROR,"attach public work-item gate");require(DetourTransactionCommit()==NO_ERROR,"commit work gate");hooked=true;gate.arm();}
        read(reader.Get());if(!native)wait(gate.captured,"capture reserved sample response");
        bool flush=!wcscmp(argv[3],L"flush"),drain=!wcscmp(argv[3],L"drain"),reentrant=!wcscmp(argv[3],L"reentrant"),eos=!wcscmp(argv[3],L"eos"),deselect=!wcscmp(argv[3],L"deselect");
        if(deselect){
            if(native){sample(callback->take());checked(reader->SetStreamSelection(audio_stream,FALSE),"deselect native audio stream");HRESULT hr=reader->ReadSample(audio_stream,0,nullptr,nullptr,nullptr,nullptr);printf("DESELECT_NATIVE return=%08lx\n",(unsigned long)hr);if(SUCCEEDED(hr)){auto rejected=callback->take_raw();require(rejected.status==MF_E_INVALIDREQUEST&&!rejected.sample,"native deselected read reports invalid request");}else require(hr==MF_E_INVALIDREQUEST,"native deselected read rejects invalid request");
                hr=reader->SetStreamSelection(audio_stream,TRUE);printf("DESELECT_NATIVE reselect=%08lx\n",(unsigned long)hr);require(hr==MF_E_INVALIDREQUEST,"native error state rejects subsequent reselection");native_error_state=true;
            }
            else{
                gate.wrap();read(reader.Get());wait(gate.complete,"later read is deferred before deselection");require(callback->count()==0,"deselection control keeps reserved response");
                checked(reader->SetStreamSelection(audio_stream,FALSE),"deselect stream with pending retry");gate.release();sample(callback->take());
                auto rejected=callback->take_raw();require(rejected.status==MF_E_INVALIDREQUEST&&!rejected.sample,"deferred read on deselected stream completes with invalid request");
            }
            if(!native){checked(reader->SetStreamSelection(audio_stream,TRUE),"reselect stream after read failure");PROPVARIANT position{};position.vt=VT_I8;position.hVal.QuadPart=0;gate.wrap();checked(reader->SetCurrentPosition(GUID_NULL,position),"seek eligibility proves deferred FIFO was released");wait(gate.complete,"accepted seek work completes");puts("DESELECT_CLEANUP terminal error and seek eligibility verified; post-error media recovery is not asserted");}
        }else if(flush){
            if(native){sample(callback->take());}else gate.wrap();
            checked(reader->Flush(audio_stream),"flush pending response");wait(callback->flushed,"flush callback completes");if(!native)wait(gate.complete,"flush internal work completes");
            unsigned baseline=callback->count();if(!native)gate.arm();read(reader.Get());
            if(!native){wait(gate.captured,"capture post-flush sample response");gate.release(true);wait(gate.complete,"stale pre-flush response work completes");require(callback->count()==baseline,"stale ready cannot consume post-flush response");gate.release();}
            sample(callback->take());require(callback->count()==baseline+1,"exactly one post-flush callback");
        }else if(reentrant){
            // Arm before release, after the first native callback if ungated.
            if(native){sample(callback->take());callback->reentrant=1;read(reader.Get());}else{callback->reentrant=1;gate.release();}
            auto a=callback->take(),b=callback->take();sample(a);sample(b);checked(callback->reentrant_status,"reentrant ReadSample succeeds");require(b.time>a.time,"reentrant read delivers next sample");
        }else{
            if(!native)gate.wrap();read(reader.Get(),drain?MF_SOURCE_READER_CONTROLF_DRAIN:0);
            if(!native){wait(gate.complete,"later read command executes before reserved response");unsigned early=callback->count();printf("GATE callbacks_before_ready=%u\n",early);gate.release();require(early==0,"later read cannot steal reserved response");}
            auto a=callback->take(),b=callback->take();sample(a);
            if(drain){require(!b.sample&&!(b.flags&MF_SOURCE_READERF_ENDOFSTREAM),"deferred DRAIN retains empty-buffer non-EOS result");read(reader.Get());auto next=callback->take();sample(next);require(next.time>a.time,"ordinary reads resume after deferred DRAIN");}
            else{sample(b);require(b.time>a.time,"queued reads deliver distinct ordered samples");}
            if(eos){
                bool ended=false;unsigned tail_samples=0,eof_callbacks=0;LONGLONG last_time=b.time;
                // The generated four-second WAV has forty native PCM packets.
                // Queue past its tail while real samples remain, not after EOF.
                for(unsigned n=0;n<64;n++)read(reader.Get());
                for(unsigned n=0;n<64;n++){auto r=callback->take();if(r.flags&MF_SOURCE_READERF_ENDOFSTREAM){require(!r.sample,"EOF callback has no sample");ended=true;++eof_callbacks;}else{require(!ended,"reserved tail samples precede every newer EOF callback");sample(r);require(r.time>last_time,"queued tail sample timestamps stay ordered");last_time=r.time;++tail_samples;}}
                require(ended&&tail_samples&&eof_callbacks,"queued batch crosses actual samples and EOF");printf("QUEUED_TAIL samples=%u eof=%u callbacks=64\n",tail_samples,eof_callbacks);
                for(unsigned n=0;n<3;n++)read(reader.Get());for(unsigned n=0;n<3;n++){auto r=callback->take();require(!r.sample&&(r.flags&MF_SOURCE_READERF_ENDOFSTREAM),"each queued EOF read completes");}
            }
        }
        if(!native_error_state){checked(reader->Flush(audio_stream),"final fixture flush");wait(callback->flushed,"final flush completes");}puts("PASS deterministic reader queue contract");exit_code=0;
    }catch(const std::exception &error){printf("FAIL %s\n",error.what());}
    // On failed assertions let the process boundary cancel owned work safely;
    // held callback refs deliberately remain alive until process termination.
    if(exit_code)ExitProcess(exit_code);
    reader.Reset();callback.Reset();if(started)MFShutdown();if(hooked){DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourDetach((PVOID*)&real_put,gated_put);DetourTransactionCommit();}if(module)FreeLibrary(module);if(initialized)CoUninitialize();return 0;
}
