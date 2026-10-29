// SPDX-License-Identifier: LGPL-2.1-or-later
#include "video-probe-common.h"
static void generate(const wchar_t *path){
    ComPtr<IMFSinkWriter> writer;checked(MFCreateSinkWriterFromURL(path,nullptr,nullptr,&writer),"create generated H.264 MP4 writer");
    ComPtr<IMFMediaType> output,input;checked(MFCreateMediaType(&output),"create encoded media type");video_type(output.Get(),MFVideoFormat_H264);
    checked(output->SetUINT32(MF_MT_AVG_BITRATE,300000),"set H.264 bitrate");checked(output->SetUINT32(MF_MT_MPEG2_PROFILE,66),"set H.264 baseline profile");
    DWORD stream=0;checked(writer->AddStream(output.Get(),&stream),"add generated H.264 stream");
    checked(MFCreateMediaType(&input),"create source media type");video_type(input.Get(),MFVideoFormat_NV12);
    checked(writer->SetInputMediaType(stream,input.Get(),nullptr),"set generated NV12 input");checked(writer->BeginWriting(),"begin generated clip");
    for(UINT i=0;i<frames;i++){
        ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFSample> sample;checked(MFCreateMemoryBuffer(width*height*3/2,&buffer),"allocate generated frame");
        BYTE *data=nullptr;checked(buffer->Lock(&data,nullptr,nullptr),"lock generated frame");
        for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++)data[y*width+x]=(BYTE)(x<width/2?40+i*4:180-i*4);
        for(UINT y=0;y<height/2;y++)for(UINT x=0;x<width;x+=2){data[width*height+y*width+x]=96;data[width*height+y*width+x+1]=160;}
        checked(buffer->Unlock(),"unlock generated frame");checked(buffer->SetCurrentLength(width*height*3/2),"set generated frame length");
        checked(MFCreateSample(&sample),"create generated sample");checked(sample->AddBuffer(buffer.Get()),"set generated sample buffer");
        checked(sample->SetSampleTime(i*duration),"set generated timestamp");checked(sample->SetSampleDuration(duration),"set generated duration");checked(writer->WriteSample(stream,sample.Get()),"encode generated sample");
    }
    checked(writer->Finalize(),"finalize generated H.264 sample");printf("GENERATED %ls frames=%u size=%ux%u fps=%u\n",path,frames,width,height,fps);
}
static bool lifecycle,async_mode,drain_test,drain_resume,wine_reader;
static decltype(&MFCreateSourceReaderFromURL) create_reader=MFCreateSourceReaderFromURL;
struct ReaderCallback : IMFSourceReaderCallback {
    LONG refs=1;
    HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr),flushed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    CRITICAL_SECTION lock;
    HRESULT status=S_OK;DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
    ReaderCallback(){InitializeCriticalSection(&lock);}
    ~ReaderCallback(){CloseHandle(ready);CloseHandle(flushed);DeleteCriticalSection(&lock);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **value)override{if(!value)return E_POINTER;*value=nullptr;if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMFSourceReaderCallback))return E_NOINTERFACE;*value=(IMFSourceReaderCallback*)this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release()override{ULONG value=InterlockedDecrement(&refs);if(!value)delete this;return value;}
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr,DWORD,DWORD sample_flags,LONGLONG timestamp,IMFSample *value)override{
        EnterCriticalSection(&lock);status=hr;flags=sample_flags;time=timestamp;sample=value;LeaveCriticalSection(&lock);SetEvent(ready);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD)override{SetEvent(flushed);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD,IMFMediaEvent*)override{return S_OK;}
    void read(IMFSourceReader *reader,DWORD control,DWORD &sample_flags,LONGLONG &timestamp,ComPtr<IMFSample> &value){
        checked(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,control,nullptr,nullptr,nullptr,nullptr),"queue asynchronous video read");
        check(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0,"asynchronous video callback completes");
        EnterCriticalSection(&lock);HRESULT hr=status;sample_flags=flags;timestamp=time;value=sample;sample.Reset();LeaveCriticalSection(&lock);checked(hr,"asynchronous video sample status");
    }
    void flush(IMFSourceReader *reader){ResetEvent(flushed);checked(reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS),"queue asynchronous decoder flush");check(WaitForSingleObject(flushed,10000)==WAIT_OBJECT_0,"asynchronous flush callback completes");}
};
struct VideoPresenter {
    HWND window=nullptr;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    ComPtr<ID3D11Texture2D> back;
    ID3D11VideoDevice *video;
    ID3D11VideoContext *context;
    ULONGLONG started=0;
    VideoPresenter(ID3D11Device *device,ID3D11VideoDevice *vd,ID3D11VideoContext *vc,IDXGIAdapter *adapter):video(vd),context(vc){
        window=CreateWindowExW(0,L"STATIC",L"Winedows H.264 hardware playback",WS_OVERLAPPEDWINDOW,40,40,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(window!=nullptr,"create video playback window");
        ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
        if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0))check(!!IsChild(GetDesktopWindow(),window),"video playback window is inside virtual desktop");
        ComPtr<IDXGIFactory> factory;checked(adapter->GetParent(IID_PPV_ARGS(&factory)),"get video presentation factory");
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=width;desc.BufferDesc.Height=height;desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;
        checked(factory->CreateSwapChain(device,&desc,&swap),"create windowed video swapchain");checked(swap->GetBuffer(0,IID_PPV_ARGS(&back)),"get video presentation surface");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;content.InputFrameRate={fps,1};content.OutputFrameRate={fps,1};content.InputWidth=width;content.InputHeight=height;content.OutputWidth=width;content.OutputHeight=height;content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        checked(video->CreateVideoProcessorEnumerator(&content,&enumerator),"enumerate native video conversion");checked(video->CreateVideoProcessor(enumerator.Get(),0,&processor),"create native GPU video processor");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};output_desc.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        checked(video->CreateVideoProcessorOutputView(back.Get(),enumerator.Get(),&output_desc,&output),"bind video processor to swapchain");
        RECT area={0,0,(LONG)width,(LONG)height};context->VideoProcessorSetStreamFrameFormat(processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);context->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&area);context->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&area);context->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&area);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};color.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;context->VideoProcessorSetStreamColorSpace(processor.Get(),0,&color);
        color.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;context->VideoProcessorSetOutputColorSpace(processor.Get(),&color);
        started=GetTickCount64();
    }
    void present(ID3D11Texture2D *texture,UINT subresource,UINT frame){
        if (!frame) started=GetTickCount64();
        ComPtr<ID3D11VideoProcessorInputView> input;D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};desc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;desc.Texture2D.ArraySlice=subresource;
        checked(video->CreateVideoProcessorInputView(texture,enumerator.Get(),&desc,&input),"bind decoded NV12 frame for GPU presentation");
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.InputFrameOrField=frame;stream.pInputSurface=input.Get();checked(context->VideoProcessorBlt(processor.Get(),output.Get(),frame,1,&stream),"convert decoded frame on GPU");
        ULONGLONG target=started+frame*1000/fps,now=GetTickCount64();if(target>now)Sleep((DWORD)(target-now));checked(swap->Present(0,0),"present decoded video frame");
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    ~VideoPresenter(){output.Reset();back.Reset();swap.Reset();if(window)DestroyWindow(window);}
};
static void decode(const wchar_t *path){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"create hardware video device");
    printf("D3D11 module: %ls; feature level=%04x\n",module_path(GetModuleHandleW(L"d3d11.dll")).c_str(),level);
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};checked(device.As(&dxgi),"query video DXGI device");checked(dxgi->GetAdapter(&adapter),"get video adapter");checked(adapter->GetDesc(&desc),"describe video adapter");printf("VIDEO ADAPTER %ls vendor=%04x device=%04x\n",desc.Description,desc.VendorId,desc.DeviceId);
    ComPtr<ID3D10Multithread> multithread;checked(device.As(&multithread),"query video multithread protection");multithread->SetMultithreadProtected(TRUE);
    ComPtr<ID3D11VideoDevice> video;ComPtr<ID3D11VideoContext> video_context;checked(device.As(&video),"query video device");checked(context.As(&video_context),"query video context");
    UINT profiles=video->GetVideoDecoderProfileCount();printf("Decoder profiles: %u\n",profiles);check(profiles>0,"hardware decoder profiles available");
    VideoPresenter presenter(device.Get(),video.Get(),video_context.Get(),adapter.Get());
    DecodeInstrumentation instrumentation(video_context.Get());
    UINT token=0;ComPtr<IMFDXGIDeviceManager> manager;checked(MFCreateDXGIDeviceManager(&token,&manager),"create video device manager");checked(manager->ResetDevice(device.Get(),token),"bind hardware video device manager");
    ComPtr<IMFAttributes> attributes;checked(MFCreateAttributes(&attributes,4),"create source-reader options");checked(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,manager.Get()),"share hardware device with decoder");
    checked(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"enable hardware transforms");checked(attributes->SetUINT32(MF_READWRITE_D3D_OPTIONAL,FALSE),"require D3D sample allocation");
    ComPtr<ReaderCallback> callback;
    if(async_mode){callback.Attach(new ReaderCallback);checked(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,callback.Get()),"select asynchronous source-reader callbacks");}
    ComPtr<IMFSourceReader> reader;checked(create_reader(path,attributes.Get(),&reader),"open H.264 source inside current namespace");
    HMODULE reader_module=nullptr;void **reader_vtable=*(void***)reader.Get();GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)reader_vtable[0],&reader_module);printf("SOURCE READER OBJECT %ls\n",module_path(reader_module).c_str());
    checked(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,FALSE),"disable unneeded streams");checked(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"select H.264 video stream");
    ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"create decoded media type");checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"set decoded major type");checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"request decoded NV12 surfaces");
    checked(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.Get()),"negotiate hardware decoded NV12");
    UINT total=0;
    for(UINT pass=0;pass<(lifecycle?5u:drain_test?2u:1u);pass++){
      UINT count=0,drained_count=0;LONGLONG previous=-1;bool eof=false,drain=false,drain_finished=false;UINT expected=lifecycle?(pass==2?5:pass==3?20:frames):frames;
      while(count<frames+4){
        DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
        DWORD control=drain_test&&!pass&&drain&&!drain_finished?MF_SOURCE_READER_CONTROLF_DRAIN:0;
        if(async_mode)callback->read(reader.Get(),control,flags,timestamp,sample);
        else checked(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,control,nullptr,&flags,&timestamp,&sample),"decode H.264 frame");
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){eof=true;break;}if(!sample){if(control&&!drain_resume)break;if(control)drain_finished=true;drain=false;continue;}
        if(control){++drained_count;if(drain_resume)drain_finished=true;}drain=true;
        ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFDXGIBuffer> surface;ComPtr<ID3D11Texture2D> texture,staging;
        checked(sample->GetBufferByIndex(0,&buffer),"get decoded sample buffer");checked(buffer.As(&surface),"decoded frame is a DXGI GPU surface");checked(surface->GetResource(IID_PPV_ARGS(&texture)),"get decoded GPU texture");UINT subresource=0;checked(surface->GetSubresourceIndex(&subresource),"get decoder texture array slice");
        D3D11_TEXTURE2D_DESC texture_desc{};texture->GetDesc(&texture_desc);check(texture_desc.Format==DXGI_FORMAT_NV12,"decoded texture format is NV12");
        // Wine's reader may copy the decoder surface into its own D3D sample
        // allocator. The successful decoder API submissions below establish
        // hardware decoding even when this final surface has shader bindings.
        printf("Decoded texture bindings: %08x\n",texture_desc.BindFlags);
        texture_desc.ArraySize=1;texture_desc.MipLevels=1;texture_desc.Usage=D3D11_USAGE_STAGING;texture_desc.BindFlags=0;texture_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;texture_desc.MiscFlags=0;
        checked(device->CreateTexture2D(&texture_desc,nullptr,&staging),"create decoded-frame readback texture");context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),subresource,nullptr);
        D3D11_MAPPED_SUBRESOURCE map{};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"read completed decoded GPU frame");
        auto pixels=(const BYTE*)map.pData;UINT left=pixels[(height/2)*map.RowPitch+width/4],right=pixels[(height/2)*map.RowPitch+width*3/4];UINT u=pixels[texture_desc.Height*map.RowPitch+(height/4)*map.RowPitch+width/2],v=pixels[texture_desc.Height*map.RowPitch+(height/4)*map.RowPitch+width/2+1];context->Unmap(staging.Get(),0);
        UINT frame=(UINT)((timestamp+duration/2)/duration);printf("DECODED frame=%u time=%lld Y=%u,%u UV=%u,%u\n",frame,timestamp,left,right,u,v);
        check((frame==count||(drain_resume&&!pass))&&frame<frames&&abs((int)left-(40+(int)frame*4))<=5&&abs((int)right-(180-(int)frame*4))<=5&&abs((int)u-96)<=5&&abs((int)v-160)<=5,"decoded pixels match generated temporal pattern");check(timestamp>previous,"decoded timestamps advance");presenter.present(texture.Get(),subresource,frame);previous=timestamp;++count;
        if(expected<frames&&count==expected)break;
      }
      printf("DECODE PASS %u frames=%u EOF=%u drain_reads=%u\n",pass,count,eof,drained_count);
      if(drain_test&&!pass){check(count>=1,"control drain completes without requesting additional source frames");if(wine_reader)check(drained_count>0,"Wine control drain returns buffered decoder frames");}
      else{check(count==expected,"expected generated frames decoded");check(expected<frames||eof,"decoder drain reaches end of stream");}
      total+=count;
      if((lifecycle&&pass<4)||(drain_test&&!pass)){
          if(pass){if(async_mode)callback->flush(reader.Get());else checked(reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS),"flush decoder and pending samples");}
          if(pass==3&&wine_reader){
              DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
              if(async_mode)callback->read(reader.Get(),0,flags,time,sample);else checked(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&time,&sample),"read after flushing decoder tail");
              check(!sample&&(flags&MF_SOURCE_READERF_ENDOFSTREAM),"flushing decoder tail preserves completed source EOF");
          }
          PROPVARIANT position{};position.vt=VT_I8;position.hVal.QuadPart=0;checked(reader->SetCurrentPosition(GUID_NULL,position),"seek decoded stream back to start");
      }
    }
    printf("HARDWARE DECODE frames=%u begin=%ld submit=%ld end=%ld\n",total,begun,submitted,ended);
    check(begun>=(LONG)total&&ended>=(LONG)total&&submitted>=(LONG)total,"successful native DXVA decode submissions prove hardware use");
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));
    if(argc!=3&&argc!=4){fprintf(stderr,"usage: video-probe --generate|--decode path.mp4; --decode-wine reader.dll path.mp4\n");return 2;}
    DEVMODEW before{};before.dmSize=sizeof(before);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before);
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))return 3;HRESULT startup=MFStartup(MF_VERSION);if(FAILED(startup)){printf("MFStartup failed %08lx\n",(ULONG)startup);CoUninitialize();return 4;}
    int result=0;try{
        lifecycle=!!wcsstr(argv[1],L"lifecycle");async_mode=!!wcsstr(argv[1],L"async");drain_test=!!wcsstr(argv[1],L"drain");drain_resume=!!wcsstr(argv[1],L"resume");wine_reader=!!wcsstr(argv[1],L"wine");
        if(argc==4){HMODULE reader=LoadLibraryExW(argv[2],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(reader!=nullptr,"load Wine Media Foundation source reader");create_reader=(decltype(create_reader))GetProcAddress(reader,"MFCreateSourceReaderFromURL");check(create_reader!=nullptr,"resolve Wine source-reader factory");printf("SOURCE READER %ls\n",module_path(reader).c_str());decode(argv[3]);}
        else if(!wcscmp(argv[1],L"--generate"))generate(argv[2]);else decode(argv[2]);
    }catch(const std::exception &error){fprintf(stderr,"FAIL video probe: %s\n",error.what());result=1;}
    MFShutdown();CoUninitialize();DEVMODEW after{};after.dmSize=sizeof(after);if(!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)||before.dmPelsWidth!=after.dmPelsWidth||before.dmPelsHeight!=after.dmPelsHeight||before.dmDisplayFrequency!=after.dmDisplayFrequency){puts("FAIL physical display mode changed");result=1;}else puts("PASS physical display mode unchanged");return result;
}
