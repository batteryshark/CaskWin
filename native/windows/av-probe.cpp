// SPDX-License-Identifier: LGPL-2.1-or-later
// Synthetic audiovisual source-reader contract test; no audio devices or capture.
#include "video-probe-common.h"
static decltype(&MFCreateSourceReaderFromURL) create_reader=MFCreateSourceReaderFromURL;
#include <cmath>
#include <deque>
#include <algorithm>

constexpr UINT audio_rate=48000,audio_channels=2,audio_block=audio_rate/fps;
constexpr double pi=3.14159265358979323846;
static void audio_type(IMFMediaType *type,REFGUID subtype){
    checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"set audio major type");
    checked(type->SetGUID(MF_MT_SUBTYPE,subtype),"set audio subtype");
    checked(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,audio_channels),"set stereo channels");
    checked(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,audio_rate),"set audio rate");
    checked(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"set PCM bit depth");
    if(subtype==MFAudioFormat_PCM){
        checked(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4),"set PCM alignment");
        checked(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,audio_rate*4),"set PCM byte rate");
    }else checked(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,24000),"set AAC bitrate");
}
static void av_generate(const wchar_t *path){
    ComPtr<IMFSinkWriter> writer;checked(MFCreateSinkWriterFromURL(path,nullptr,nullptr,&writer),"create generated audiovisual MP4 writer");
    ComPtr<IMFMediaType> vout,vin,aout,ain;DWORD vs=0,as=0;
    checked(MFCreateMediaType(&vout),"create H264 type");video_type(vout.Get(),MFVideoFormat_H264);
    checked(vout->SetUINT32(MF_MT_AVG_BITRATE,300000),"set H264 bitrate");checked(vout->SetUINT32(MF_MT_MPEG2_PROFILE,66),"set baseline profile");
    checked(writer->AddStream(vout.Get(),&vs),"add H264 video");
    checked(MFCreateMediaType(&vin),"create NV12 type");video_type(vin.Get(),MFVideoFormat_NV12);checked(writer->SetInputMediaType(vs,vin.Get(),nullptr),"set NV12 writer input");
    checked(MFCreateMediaType(&aout),"create AAC type");audio_type(aout.Get(),MFAudioFormat_AAC);checked(writer->AddStream(aout.Get(),&as),"add AAC audio");
    checked(MFCreateMediaType(&ain),"create PCM type");audio_type(ain.Get(),MFAudioFormat_PCM);checked(writer->SetInputMediaType(as,ain.Get(),nullptr),"set generated PCM writer input");
    checked(writer->BeginWriting(),"begin audiovisual MP4");
    for(UINT i=0;i<frames;i++){
        ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFSample> sample;BYTE *data=nullptr;
        checked(MFCreateMemoryBuffer(width*height*3/2,&buffer),"allocate synthetic image");checked(buffer->Lock(&data,nullptr,nullptr),"lock synthetic image");
        for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++)data[y*width+x]=(BYTE)(x<width/2?40+i*4:180-i*4);
        for(UINT y=0;y<height/2;y++)for(UINT x=0;x<width;x+=2){data[width*height+y*width+x]=96;data[width*height+y*width+x+1]=160;}
        checked(buffer->Unlock(),"unlock image");checked(buffer->SetCurrentLength(width*height*3/2),"size image");
        checked(MFCreateSample(&sample),"create image sample");checked(sample->AddBuffer(buffer.Get()),"attach image");checked(sample->SetSampleTime(i*duration),"timestamp image");checked(sample->SetSampleDuration(duration),"duration image");checked(writer->WriteSample(vs,sample.Get()),"encode H264 image");
        sample.Reset();buffer.Reset();checked(MFCreateMemoryBuffer(audio_block*4,&buffer),"allocate generated PCM");checked(buffer->Lock(&data,nullptr,nullptr),"lock generated PCM");
        auto pcm=(short*)data;for(UINT n=0;n<audio_block;n++)for(UINT channel=0;channel<2;channel++)pcm[n*2+channel]=(short)std::lround(8192*std::sin(2*pi*(channel?660:440)*(i*audio_block+n)/audio_rate));
        checked(buffer->Unlock(),"unlock PCM");checked(buffer->SetCurrentLength(audio_block*4),"size PCM");checked(MFCreateSample(&sample),"create PCM sample");checked(sample->AddBuffer(buffer.Get()),"attach PCM");
        checked(sample->SetSampleTime((LONGLONG)i*audio_block*10000000/audio_rate),"timestamp PCM");checked(sample->SetSampleDuration((LONGLONG)audio_block*10000000/audio_rate),"duration PCM");checked(writer->WriteSample(as,sample.Get()),"encode AAC audio");
    }
    checked(writer->Finalize(),"finalize generated audiovisual MP4");printf("GENERATED AV video=%u audio_frames=%u rate=%u channels=2\n",frames,frames*audio_block,audio_rate);
}
struct AVResult {HRESULT hr=S_OK;DWORD stream=0,flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;};
struct AVCallback : IMFSourceReaderCallback {
    LONG refs=1;HANDLE ready=CreateSemaphoreW(nullptr,0,256,nullptr),flushed=CreateEventW(nullptr,FALSE,FALSE,nullptr);CRITICAL_SECTION lock;std::deque<AVResult> results;
    AVCallback(){InitializeCriticalSection(&lock);}~AVCallback(){CloseHandle(ready);CloseHandle(flushed);DeleteCriticalSection(&lock);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **p)override{if(!p)return E_POINTER;*p=nullptr;if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMFSourceReaderCallback))return E_NOINTERFACE;*p=(IMFSourceReaderCallback*)this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs);}ULONG STDMETHODCALLTYPE Release()override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr,DWORD stream,DWORD flags,LONGLONG time,IMFSample *sample)override{EnterCriticalSection(&lock);results.push_back({hr,stream,flags,time,sample});LeaveCriticalSection(&lock);ReleaseSemaphore(ready,1,nullptr);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD)override{SetEvent(flushed);return S_OK;}HRESULT STDMETHODCALLTYPE OnEvent(DWORD,IMFMediaEvent*)override{return S_OK;}
    AVResult take(){check(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0,"asynchronous audiovisual read completes");EnterCriticalSection(&lock);auto r=std::move(results.front());results.pop_front();LeaveCriticalSection(&lock);return r;}
};
static std::wstring object_module(IUnknown *object){HMODULE m=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(*(void***)object)[0],&m);return module_path(m);}
using SourceEndEvent=HRESULT(STDMETHODCALLTYPE*)(IMFMediaSource*,IMFAsyncResult*,IMFMediaEvent**);
static SourceEndEvent real_source_end;
static IMFMediaSource *gated_source;
static HANDLE start_observed,start_release;
static LONG start_gate_used;
static HRESULT STDMETHODCALLTYPE source_end_event(IMFMediaSource *source,IMFAsyncResult *result,IMFMediaEvent **event){
    HRESULT hr=real_source_end(source,result,event);MediaEventType type=MEUnknown;
    if(SUCCEEDED(hr)&&source==gated_source&&SUCCEEDED((*event)->GetType(&type))&&type==MESourceStarted&&!InterlockedExchange(&start_gate_used,1)){
        SetEvent(start_observed);if(WaitForSingleObject(start_release,10000)!=WAIT_OBJECT_0)puts("FAIL source start acknowledgement gate timed out");
    }
    return hr;
}
struct SourceStartGate {
    ComPtr<IMFMediaSource> source;
    explicit SourceStartGate(IMFSourceReader *reader){
        checked(reader->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE,GUID_NULL,IID_PPV_ARGS(&source)),"get actual source for start acknowledgement gate");gated_source=source.Get();start_gate_used=0;start_observed=CreateEventW(nullptr,TRUE,FALSE,nullptr);start_release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        real_source_end=(SourceEndEvent)(*(void***)source.Get())[5];check(DetourTransactionBegin()==NO_ERROR,"begin source event gate");DetourUpdateThread(GetCurrentThread());check(DetourAttach((PVOID*)&real_source_end,source_end_event)==NO_ERROR,"gate first source started acknowledgement");check(DetourTransactionCommit()==NO_ERROR,"commit source event gate");
    }
    void release(){SetEvent(start_release);}
    ~SourceStartGate(){release();DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourDetach((PVOID*)&real_source_end,source_end_event);DetourTransactionCommit();CloseHandle(start_observed);CloseHandle(start_release);gated_source=nullptr;}
};
static DWORD WINAPI release_source_gate(void*){Sleep(250);SetEvent(start_release);return 0;}
static void inspect_chain(IMFSourceReader *reader,DWORD stream,bool video){
    ComPtr<IMFSourceReaderEx> ex;checked(reader->QueryInterface(IID_PPV_ARGS(&ex)),"query source reader transform chain");UINT count=0;bool decoder=false,processor=false;
    for(;count<8;count++){ComPtr<IMFTransform> transform;GUID category{};HRESULT hr=ex->GetTransformForStream(stream,count,&category,&transform);if(hr==MF_E_INVALIDINDEX)break;checked(hr,"get actual stream transform");auto path=object_module(transform.Get());printf("AV TRANSFORM stream=%lu index=%u module=%ls\n",stream,count,path.c_str());
        if(video){decoder|=category==MFT_CATEGORY_VIDEO_DECODER;processor|=category==MFT_CATEGORY_VIDEO_PROCESSOR;}else decoder|=category==MFT_CATEGORY_AUDIO_DECODER;
    }
    check(decoder,"actual decoder appears in source-reader chain");if(video)check(count>=2&&processor,"actual video chain includes decoder and color processor");
}
struct AVStats {
    UINT videos=0,audio_samples=0;UINT64 pcm_frames=0;LONGLONG video_time=-1,audio_time=-1,audio_end=-1;UINT tone_checks=0;
    void audio(IMFSample *sample,LONGLONG timestamp){
        check(timestamp>audio_time,"audio timestamps advance");if(audio_end>=0)check(std::llabs(timestamp-audio_end)<100,"audio timestamps have no gaps");audio_time=timestamp;
        ComPtr<IMFMediaBuffer> buffer;checked(sample->ConvertToContiguousBuffer(&buffer),"get decoded PCM buffer");BYTE *data=nullptr;DWORD size=0;checked(buffer->Lock(&data,nullptr,&size),"lock decoded PCM");check(size&&!(size%4),"decoded stereo PCM frame alignment");UINT n=size/4;
        // AAC priming and the terminal padding may contain silence. Interior
        // blocks must retain the independently generated channel frequencies.
        if(timestamp>500000&&timestamp<18000000&&n>=512){
            auto pcm=(const short*)data;for(UINT channel=0;channel<2;channel++){
                double energy=0,cs=0,sn=0,wrong_cs=0,wrong_sn=0;double frequency=channel?660:440;
                for(UINT j=0;j<n;j++){double value=pcm[j*2+channel];double phase=2*pi*frequency*j/audio_rate,wrong=2*pi*(channel?440:660)*j/audio_rate;energy+=value*value;cs+=value*std::cos(phase);sn+=value*std::sin(phase);wrong_cs+=value*std::cos(wrong);wrong_sn+=value*std::sin(wrong);}
                double amplitude=2*std::sqrt(cs*cs+sn*sn)/n,wrong=2*std::sqrt(wrong_cs*wrong_cs+wrong_sn*wrong_sn)/n,rms=std::sqrt(energy/n);
                if(!(amplitude>6500&&amplitude<9500&&rms>4500&&rms<7000&&wrong<amplitude*.25))printf("AUDIO SIGNAL channel=%u amplitude=%.2f rms=%.2f wrong=%.2f\n",channel,amplitude,rms,wrong);
                check(amplitude>6500&&amplitude<9500&&rms>4500&&rms<7000&&wrong<amplitude*.25,"AAC decoded channel preserves generated tone and level");tone_checks++;
            }
        }
        checked(buffer->Unlock(),"unlock PCM");audio_end=timestamp+(LONGLONG)n*10000000/audio_rate;pcm_frames+=n;audio_samples++;
    }
    void video(IMFSample *sample,LONGLONG timestamp,ID3D11Device *device,ID3D11DeviceContext *context){
        check(timestamp>video_time,"video timestamps advance");video_time=timestamp;UINT frame=(UINT)((timestamp+duration/2)/duration);check(frame<frames,"video timestamp maps to generated frame");
        ComPtr<IMFMediaBuffer> buffer;checked(sample->GetBufferByIndex(0,&buffer),"get converted video buffer");ComPtr<IMFDXGIBuffer> surface;BYTE *data=nullptr;LONG pitch=0;ComPtr<IMF2DBuffer> plane;ComPtr<ID3D11Texture2D> texture,staging;bool gpu=SUCCEEDED(buffer.As(&surface));
        if(gpu){UINT index=0;checked(surface->GetResource(IID_PPV_ARGS(&texture)),"get converted GPU texture");checked(surface->GetSubresourceIndex(&index),"get converted texture index");D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);check(desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM,"color processor produces BGRA GPU surface");desc.ArraySize=1;desc.MipLevels=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;checked(device->CreateTexture2D(&desc,nullptr,&staging),"create converted frame readback");context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),index,nullptr);D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"map converted frame");data=(BYTE*)mapped.pData;pitch=(LONG)mapped.RowPitch;
        }else if(SUCCEEDED(buffer.As(&plane)))checked(plane->Lock2D(&data,&pitch),"lock converted RGB image");else{checked(buffer->Lock(&data,nullptr,nullptr),"lock converted RGB bytes");pitch=width*4;}
        for(UINT side=0;side<2;side++){int y=side?180-(int)frame*4:40+(int)frame*4;int c=y-16,d=-32,e=32;int rgb[3]={((298*c+516*d+128)>>8),((298*c-100*d-208*e+128)>>8),((298*c+409*e+128)>>8)};BYTE *pixel=data+(height/2)*pitch+(side?3*width/4:width/4)*4;for(UINT k=0;k<3;k++){int expected=std::max(0,std::min(255,rgb[k]));if(std::abs((int)pixel[k]-expected)>7)printf("RGB frame=%u side=%u channel=%u actual=%u expected=%d\n",frame,side,k,pixel[k],expected);check(std::abs((int)pixel[k]-expected)<=7,"color-converted pixel matches generated H264 frame");}}
        if(gpu)context->Unmap(staging.Get(),0);else if(plane)checked(plane->Unlock2D(),"unlock converted RGB image");else checked(buffer->Unlock(),"unlock RGB bytes");videos++;
    }
};
static void av_decode(const wchar_t *path,bool asynchronous,bool full_lifecycle,bool control_drain,bool outstanding,bool selection){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"create audiovisual hardware device");
    ComPtr<ID3D10Multithread> multithread;checked(device.As(&multithread),"get device multithread lock");multithread->SetMultithreadProtected(TRUE);
    ComPtr<ID3D11VideoContext> vc;checked(context.As(&vc),"get native video context");DecodeInstrumentation instrumentation(vc.Get());
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};checked(device.As(&dxgi),"get AV DXGI device");checked(dxgi->GetAdapter(&adapter),"get AV adapter");checked(adapter->GetDesc(&desc),"describe AV adapter");printf("AV ADAPTER %ls vendor=%04x device=%04x\n",desc.Description,desc.VendorId,desc.DeviceId);
    UINT token=0;ComPtr<IMFDXGIDeviceManager> manager;checked(MFCreateDXGIDeviceManager(&token,&manager),"create AV device manager");checked(manager->ResetDevice(device.Get(),token),"bind AV device manager");ComPtr<IMFAttributes> attributes;checked(MFCreateAttributes(&attributes,5),"create AV reader options");checked(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,manager.Get()),"set AV decoder device");checked(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"enable AV hardware transforms");checked(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE),"enable reader color conversion");
    ComPtr<AVCallback> callback;if(asynchronous){callback.Attach(new AVCallback);checked(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get()),"enable AV async callbacks");}
    ComPtr<IMFSourceReader> reader;checked(create_reader(path,attributes.Get(),&reader),"open generated audiovisual MP4");printf("SOURCE READER OBJECT %ls\n",object_module(reader.Get()).c_str());
    DWORD vs=(DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX,as=(DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX;for(DWORD stream=0;stream<8;stream++){ComPtr<IMFMediaType> type;HRESULT hr=reader->GetNativeMediaType(stream,0,&type);if(hr==MF_E_INVALIDSTREAMNUMBER)break;checked(hr,"inspect source stream");GUID major{},subtype{};checked(type->GetMajorType(&major),"get source major type");checked(type->GetGUID(MF_MT_SUBTYPE,&subtype),"get source subtype");if(major==MFMediaType_Video){vs=stream;check(subtype==MFVideoFormat_H264,"source video is H264");}else if(major==MFMediaType_Audio){as=stream;check(subtype==MFAudioFormat_AAC,"source audio is AAC");}}
    check(vs!=(DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX&&as!=(DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX&&vs!=as,"MP4 contains separate audio and video streams");checked(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,TRUE),"select both source streams");
    ComPtr<IMFMediaType> vtype,atype;checked(MFCreateMediaType(&vtype),"create converted video type");video_type(vtype.Get(),MFVideoFormat_RGB32);checked(reader->SetCurrentMediaType(vs,nullptr,vtype.Get()),"negotiate decoder and RGB color processor");checked(MFCreateMediaType(&atype),"create decoded audio type");audio_type(atype.Get(),MFAudioFormat_PCM);checked(reader->SetCurrentMediaType(as,nullptr,atype.Get()),"negotiate AAC decoder to stereo PCM");inspect_chain(reader.Get(),vs,true);inspect_chain(reader.Get(),as,false);
    UINT total_video=0;UINT64 total_pcm=0;
    auto read=[&](DWORD requested,DWORD control){AVResult r;if(callback){checked(reader->ReadSample(requested,control,nullptr,nullptr,nullptr,nullptr),"queue audiovisual read");r=callback->take();}else r.hr=reader->ReadSample(requested,control,&r.stream,&r.flags,&r.time,&r.sample);checked(r.hr,"read audiovisual sample");return r;};
    if(selection){
        check(callback!=nullptr,"selection boundary uses async reader");SourceStartGate gate(reader.Get());
        checked(reader->SetStreamSelection(as,FALSE),"initially deselect audio");checked(reader->ReadSample(vs,0,nullptr,nullptr,nullptr,nullptr),"queue first video read");
        check(WaitForSingleObject(start_observed,10000)==WAIT_OBJECT_0,"native source started before reader acknowledgement");HANDLE release=CreateThread(nullptr,0,release_source_gate,nullptr,0,nullptr);check(release!=nullptr,"schedule source acknowledgement release");checked(reader->SetStreamSelection(as,TRUE),"select audio while start acknowledgement pending");checked(reader->ReadSample(as,0,nullptr,nullptr,nullptr,nullptr),"queue new stream during initial start");if(object_module(reader.Get()).find(L"\\builtin\\")!=std::wstring::npos)check(WaitForSingleObject(start_release,0)==WAIT_TIMEOUT,"Wine selection applied before pending start acknowledgement");check(WaitForSingleObject(release,10000)==WAIT_OBJECT_0,"source acknowledgement release completes");CloseHandle(release);
        AVStats stats;UINT complete=0,video_completions=0,audio_completions=0;
        for(UINT n=0;n<8&&complete<2;n++){auto r=callback->take();checked(r.hr,"selection boundary callback status");check(r.stream==vs||r.stream==as,"selection callback has known index");if(!r.sample){printf("AV SELECTION EMPTY stream=%lu flags=%08lx\n",r.stream,r.flags);check(!(r.flags&MF_SOURCE_READERF_ERROR),"selection change has no stream error");if(!(r.flags&MF_SOURCE_READERF_ENDOFSTREAM)){checked(reader->ReadSample(r.stream,0,nullptr,nullptr,nullptr,nullptr),"read after selection stream tick");continue;}}else if(r.stream==vs)stats.video(r.sample.Get(),r.time,device.Get(),context.Get());else stats.audio(r.sample.Get(),r.time);++complete;if(r.stream==vs)++video_completions;else ++audio_completions;}
        check(video_completions==1&&audio_completions==1,"pending selection change completes both stream requests");total_video+=stats.videos;total_pcm+=stats.pcm_frames;
        PROPVARIANT position{};position.vt=VT_I8;checked(reader->SetCurrentPosition(GUID_NULL,position),"seek after selection change");
    }
    if(control_drain){
        AVStats stats;
        for(DWORD stream:{vs,as}){
            auto first=read(stream,0);check(first.stream==stream&&first.sample,"explicit stream read returns its sample");if(stream==vs)stats.video(first.sample.Get(),first.time,device.Get(),context.Get());else stats.audio(first.sample.Get(),first.time);first.sample.Reset();
            UINT drained=0;bool complete=false;
            for(UINT n=0;n<128;n++){auto r=read(stream,MF_SOURCE_READER_CONTROLF_DRAIN);check(r.stream==stream,"control drain keeps requested stream");if(!r.sample){complete=true;break;}if(stream==vs)stats.video(r.sample.Get(),r.time,device.Get(),context.Get());else stats.audio(r.sample.Get(),r.time);++drained;}
            check(complete,"stream control drain terminates without new source requests");printf("AV CONTROL DRAIN stream=%lu samples=%u\n",stream,drained);
        }
        total_video+=stats.videos;total_pcm+=stats.pcm_frames;
        PROPVARIANT position{};position.vt=VT_I8;checked(reader->SetCurrentPosition(GUID_NULL,position),"seek both streams after control drain");
    }
    if(outstanding){
        check(callback!=nullptr,"queued-read stress uses async reader");
        // Queue independent demand on both streams before consuming callbacks.
        // Four video surfaces remain below the backend allocator capacity while
        // exercising multiple requests through a two-transform chain.
        AVStats stats;
        for(UINT batch=0;batch<3;batch++){
            for(UINT n=0;n<4;n++){checked(reader->ReadSample(vs,0,nullptr,nullptr,nullptr,nullptr),"queue outstanding video demand");checked(reader->ReadSample(as,0,nullptr,nullptr,nullptr,nullptr),"queue outstanding audio demand");}
            for(UINT n=0;n<8;n++){auto r=callback->take();checked(r.hr,"outstanding audiovisual request status");check(r.sample!=nullptr,"outstanding request returns sample");if(r.stream==vs)stats.video(r.sample.Get(),r.time,device.Get(),context.Get());else{check(r.stream==as,"outstanding callback has selected index");stats.audio(r.sample.Get(),r.time);}}
            check(stats.videos==(batch+1)*4&&stats.audio_samples==(batch+1)*4,"all outstanding requests complete exactly once");
        }
        total_video+=stats.videos;total_pcm+=stats.pcm_frames;
        PROPVARIANT position{};position.vt=VT_I8;checked(reader->SetCurrentPosition(GUID_NULL,position),"seek after outstanding multistream reads");
    }
    for(UINT pass=0;pass<(full_lifecycle?4u:1u);pass++){
        AVStats stats;bool veof=false,aeof=false;UINT empty=0;
        while(!veof||!aeof){auto r=read((DWORD)MF_SOURCE_READER_ANY_STREAM,0);check(r.stream==vs||r.stream==as,"ANY_STREAM reports selected stream index");if(r.flags&MF_SOURCE_READERF_ENDOFSTREAM){if(r.stream==vs)veof=true;else aeof=true;}if(!r.sample){check(++empty<32,"multistream reads make progress");continue;}empty=0;if(r.stream==vs)stats.video(r.sample.Get(),r.time,device.Get(),context.Get());else stats.audio(r.sample.Get(),r.time);
            if(full_lifecycle&&pass==2&&stats.videos>=5&&stats.audio_samples>=8)break;
        }
        bool partial=full_lifecycle&&pass==2;printf("AV PASS %u video=%u audio_samples=%u pcm_frames=%llu tone_checks=%u video_eof=%u audio_eof=%u\n",pass,stats.videos,stats.audio_samples,stats.pcm_frames,stats.tone_checks,veof,aeof);
        if(!partial){check(veof&&aeof,"both streams reach EOF");check(stats.videos==frames,"all generated video frames survive conversion");check(stats.pcm_frames>=frames*audio_block&&stats.pcm_frames<=frames*audio_block+4096,"AAC duration includes only bounded encoder padding");check(stats.tone_checks>=100,"decoded audio signal verified throughout clip");}total_video+=stats.videos;total_pcm+=stats.pcm_frames;
        if(full_lifecycle&&pass<3){if(pass){if(callback){ResetEvent(callback->flushed);checked(reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS),"flush both streams");check(WaitForSingleObject(callback->flushed,10000)==WAIT_OBJECT_0,"multistream flush completes");}else checked(reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS),"flush both streams");}PROPVARIANT position{};position.vt=VT_I8;checked(reader->SetCurrentPosition(GUID_NULL,position),"seek both streams to zero");}
    }
    printf("AV HARDWARE video=%u pcm_frames=%llu begin=%ld submit=%ld end=%ld\n",total_video,total_pcm,begun,submitted,ended);check(begun>=(LONG)total_video&&submitted>=(LONG)total_video&&ended>=(LONG)total_video,"real native DXVA submissions accompany converted video");
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));if(argc<3||argc>4)return 2;
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 3;hr=MFStartup(MF_VERSION);if(FAILED(hr)){printf("FAIL MFStartup %08lx\n",(ULONG)hr);return 4;}int result=0;
    try{if(!wcscmp(argv[1],L"--generate"))av_generate(argv[2]);else{if(argc==4){HMODULE m=LoadLibraryExW(argv[2],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(m!=nullptr,"load explicit source reader");create_reader=(decltype(create_reader))GetProcAddress(m,"MFCreateSourceReaderFromURL");check(create_reader!=nullptr,"resolve explicit reader");}av_decode(argv[argc-1],!!wcsstr(argv[1],L"async"),!!wcsstr(argv[1],L"lifecycle"),!!wcsstr(argv[1],L"drain"),!!wcsstr(argv[1],L"queued"),!!wcsstr(argv[1],L"selection"));}}
    catch(const std::exception &e){printf("FAIL AV probe: %s\n",e.what());result=1;}MFShutdown();CoUninitialize();return result;
}
