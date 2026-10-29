// SPDX-License-Identifier: LGPL-2.1-or-later
// Generated PNG/JPEG -> real D3DX decoding -> HAL texture draw -> pixel readback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static constexpr UINT width=16,height=16;
static int failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK){printf("%s %s (hr=%08lx)\n",ok?"PASS":"FAIL",name,(unsigned long)hr);if(!ok)++failures;}
static DWORD WINAPI watchdog(void*){Sleep(60000);fputs("FAIL imaging fixture exceeded 60 seconds\n",stderr);TerminateProcess(GetCurrentProcess(),124);return 124;}
static std::wstring module_path(HMODULE module){wchar_t path[32768];DWORD n=GetModuleFileNameW(module,path,32768);return n&&n<32768?std::wstring(path,n):L"";}
static bool same_file(const std::wstring &a,const std::wstring &b){
    HANDLE x=CreateFileW(a.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr),y=CreateFileW(b.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION p{},q{};bool equal=x!=INVALID_HANDLE_VALUE&&y!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(x,&p)&&GetFileInformationByHandle(y,&q)&&p.dwVolumeSerialNumber==q.dwVolumeSerialNumber&&p.nFileIndexHigh==q.nFileIndexHigh&&p.nFileIndexLow==q.nFileIndexLow;
    if(x!=INVALID_HANDLE_VALUE)CloseHandle(x);if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);return equal;
}
static bool file_bytes(const std::wstring &path,std::vector<BYTE> &bytes,bool write){
    HANDLE file=CreateFileW(path.c_str(),write?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ,nullptr,write?CREATE_NEW:OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    bool ok=true;DWORD done=0;
    if(!write){LARGE_INTEGER size{};ok=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<1024*1024;if(ok)bytes.resize((size_t)size.QuadPart);}
    if(ok)ok=(write?WriteFile(file,bytes.data(),(DWORD)bytes.size(),&done,nullptr):ReadFile(file,bytes.data(),(DWORD)bytes.size(),&done,nullptr))&&done==bytes.size();
    CloseHandle(file);return ok;
}
static IWICImagingFactory *wic_factory(){
    HMODULE module=LoadLibraryW(L"windowscodecs.dll");
    using Create=HRESULT(WINAPI*)(UINT,IWICImagingFactory**);auto create=module?(Create)GetProcAddress(module,"WICCreateImagingFactory_Proxy"):nullptr;
    IWICImagingFactory *factory=nullptr;HRESULT hr=create?create(WINCODEC_SDK_VERSION,&factory):E_NOINTERFACE;check(SUCCEEDED(hr)&&factory,"native WIC imaging factory",hr);
    if(module){wprintf(L"WIC provider: %ls\n",module_path(module).c_str());/* Keep provider alive while its COM objects exist. */}
    return factory;
}
static std::vector<BYTE> source_pixels(bool jpeg){
    std::vector<BYTE> pixels(width*height*4);
    const DWORD colors[]={0xffc02030,0xff30b040,0xff4050c0,0xffc0b090};
    for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++){
        DWORD pixel=jpeg?colors[(y/8)*2+x/8]:((32+(x*11+y*7)%224)<<24)|((x*13+9)<<16)|((y*13+7)<<8)|((x*3+y*5+23)&255);
        memcpy(pixels.data()+(y*width+x)*4,&pixel,4);
    }return pixels;
}
static bool decode_reference(IWICImagingFactory *factory,const std::vector<BYTE> &encoded,std::vector<BYTE> &pixels){
    IWICStream *stream=nullptr;IWICBitmapDecoder *decoder=nullptr;IWICBitmapFrameDecode *frame=nullptr;IWICFormatConverter *converter=nullptr;
    HRESULT hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),(DWORD)encoded.size());
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(frame,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    pixels.resize(width*height*4);
    if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,(UINT)pixels.size(),pixels.data());
    check(SUCCEEDED(hr),"decode generated image into independent host reference pixels",hr);
    if(converter)converter->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();return SUCCEEDED(hr);
}
static bool close_pixels(const BYTE *pixels,UINT pitch,const std::vector<BYTE> &expected,unsigned tolerance){
    if(!pixels||pitch<width*4||expected.size()!=width*height*4)return false;
    unsigned maximum=0,alpha_delta=0;UINT first_y=0,first_x=0;
    for(UINT y=0;y<height;y++)for(UINT x=0;x<width*4;x++){unsigned delta=(unsigned)abs(int(pixels[y*pitch+x])-int(expected[y*width*4+x]));if(x%4==3)alpha_delta=std::max(alpha_delta,delta);if(delta>maximum){maximum=delta;first_y=y;first_x=x;}}
    printf("Pixel comparison: maximum_delta=%u alpha_delta=%u RGB_tolerance=%u\n",maximum,alpha_delta,tolerance);
    if(maximum>tolerance||alpha_delta)printf("Pixel mismatch: row=%u byte=%u got=%u expected=%u\n",first_y,first_x,pixels[first_y*pitch+first_x],expected[first_y*width*4+first_x]);
    return maximum<=tolerance&&!alpha_delta;
}
static void generate(const wchar_t *directory){
    IWICImagingFactory *factory=wic_factory();if(!factory)return;
    for(bool jpeg:{false,true}){
        const wchar_t *name=jpeg?L"fixture.jpg":L"fixture.png";const GUID &format=jpeg?GUID_ContainerFormatJpeg:GUID_ContainerFormatPng;
        auto source=source_pixels(jpeg);std::vector<BYTE> packed;
        if(jpeg){packed.resize(width*height*3);for(UINT i=0;i<width*height;i++)memcpy(packed.data()+i*3,source.data()+i*4,3);}else packed=source;
        IStream *stream=nullptr;IWICBitmapEncoder *encoder=nullptr;IWICBitmapFrameEncode *frame=nullptr;IPropertyBag2 *options=nullptr;
        HRESULT hr=CreateStreamOnHGlobal(nullptr,TRUE,&stream);
        if(SUCCEEDED(hr))hr=factory->CreateEncoder(format,nullptr,&encoder);
        if(SUCCEEDED(hr))hr=encoder->Initialize(stream,WICBitmapEncoderNoCache);
        if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,&options);
        if(SUCCEEDED(hr)&&jpeg){PROPBAG2 property{};property.pstrName=const_cast<LPOLESTR>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=1.0f;hr=options->Write(1,&property,&quality);}
        if(SUCCEEDED(hr))hr=frame->Initialize(options);
        if(SUCCEEDED(hr))hr=frame->SetSize(width,height);
        WICPixelFormatGUID pixel_format=jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;
        if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&pixel_format);
        bool exact_format=IsEqualGUID(pixel_format,jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA);
        check(exact_format,"encoder accepts requested source format");
        if(SUCCEEDED(hr)&&exact_format)hr=frame->WritePixels(height,width*(jpeg?3:4),(UINT)packed.size(),packed.data());
        if(SUCCEEDED(hr))hr=frame->Commit();if(SUCCEEDED(hr))hr=encoder->Commit();
        check(SUCCEEDED(hr),jpeg?"generate owned JPEG fixture":"generate owned alpha PNG fixture",hr);
        if(SUCCEEDED(hr)){
            STATSTG stat{};hr=stream->Stat(&stat,STATFLAG_NONAME);std::vector<BYTE> encoded((size_t)stat.cbSize.QuadPart);LARGE_INTEGER zero{};ULONG read=0;
            if(SUCCEEDED(hr))hr=stream->Seek(zero,STREAM_SEEK_SET,nullptr);
            if(SUCCEEDED(hr))hr=stream->Read(encoded.data(),(ULONG)encoded.size(),&read);
            check(SUCCEEDED(hr)&&read==encoded.size(),"read encoded fixture bytes",hr);
            std::vector<BYTE> reference;
            if(SUCCEEDED(hr)&&decode_reference(factory,encoded,reference)){
                if(!jpeg)check(reference==source,"lossless PNG reference preserves every channel and alpha");
                else{
                    bool centers=true;for(UINT y:{4u,12u})for(UINT x:{4u,12u})for(UINT c=0;c<3;c++)centers=centers&&abs(int(reference[(y*width+x)*4+c])-int(source[(y*width+x)*4+c]))<=8;
                    check(centers,"JPEG reference preserves known quadrant colors within lossy tolerance");
                }
                std::wstring path=std::wstring(directory)+L"\\"+name;
                check(file_bytes(path,encoded,true),"save generated image in fixture directory",HRESULT_FROM_WIN32(GetLastError()));
                check(file_bytes(path+L".bgra",reference,true),"save independent host decode reference",HRESULT_FROM_WIN32(GetLastError()));
            }
        }
        if(options)options->Release();if(frame)frame->Release();if(encoder)encoder->Release();if(stream)stream->Release();
    }factory->Release();
}
static void draw_and_read(IDirect3DDevice9 *device,IDirect3DTexture9 *texture,const std::vector<BYTE> &expected,unsigned tolerance){
    IDirect3DSurface9 *target=nullptr,*readback=nullptr,*previous=nullptr;
    HRESULT hr=device->GetRenderTarget(0,&previous);
    if(SUCCEEDED(hr))hr=device->CreateRenderTarget(width,height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr);
    if(SUCCEEDED(hr))hr=device->SetRenderTarget(0,target);
    if(SUCCEEDED(hr))hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,0x01020304,1,0);
    struct Vertex{float x,y,z,rhw,u,v;};const Vertex quad[]={{-.5f,-.5f,.5f,1,0,0},{width-.5f,-.5f,.5f,1,1,0},{-.5f,height-.5f,.5f,1,0,1},{width-.5f,height-.5f,.5f,1,1,1}};
    if(SUCCEEDED(hr))hr=device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    if(SUCCEEDED(hr))hr=device->SetTexture(0,texture);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_ZENABLE,FALSE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_LIGHTING,FALSE);
    if(SUCCEEDED(hr))hr=device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    if(SUCCEEDED(hr))hr=device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
    if(SUCCEEDED(hr))hr=device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    if(SUCCEEDED(hr))hr=device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    if(SUCCEEDED(hr))hr=device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    if(SUCCEEDED(hr))hr=device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    if(SUCCEEDED(hr))hr=device->BeginScene();
    if(SUCCEEDED(hr)){HRESULT draw=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex));HRESULT end=device->EndScene();hr=FAILED(draw)?draw:end;}
    check(SUCCEEDED(hr),"draw decoded image texture on D3D9 HAL",hr);
    if(SUCCEEDED(hr))hr=device->CreateOffscreenPlainSurface(width,height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr);
    if(SUCCEEDED(hr))hr=device->GetRenderTargetData(target,readback);
    D3DLOCKED_RECT lock{};if(SUCCEEDED(hr))hr=readback->LockRect(&lock,nullptr,D3DLOCK_READONLY);
    check(SUCCEEDED(hr)&&close_pixels((BYTE*)lock.pBits,(UINT)lock.Pitch,expected,tolerance),"GPU readback matches all image pixels including alpha",hr);
    if(SUCCEEDED(hr))readback->UnlockRect();
    device->SetTexture(0,nullptr);if(previous){device->SetRenderTarget(0,previous);previous->Release();}
    if(readback)readback->Release();if(target)target->Release();
}
static void texture_tests(const wchar_t *directory,const wchar_t *expected_helper,const wchar_t *expected_d3d){
    HMODULE helper=LoadLibraryW(L"d3dx9_43.dll"),graphics=LoadLibraryW(L"d3d9.dll");
    check(helper&&same_file(module_path(helper),expected_helper),"selected D3DX helper file identity");
    check(graphics&&same_file(module_path(graphics),expected_d3d),"selected D3D9 file identity");
    if(!helper||!graphics)return;
    auto metadata=(decltype(&D3DXGetImageInfoFromFileInMemory))GetProcAddress(helper,"D3DXGetImageInfoFromFileInMemory");
    auto load=(decltype(&D3DXCreateTextureFromFileInMemoryEx))GetProcAddress(helper,"D3DXCreateTextureFromFileInMemoryEx");
    auto create=(decltype(&Direct3DCreate9))GetProcAddress(graphics,"Direct3DCreate9");
    check(metadata&&load&&create,"D3DX image and D3D9 exports available");if(!metadata||!load||!create)return;
    HWND window=CreateWindowExW(0,L"STATIC",L"Winedows image fixture",WS_OVERLAPPEDWINDOW,0,0,80,80,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    IDirect3D9 *api=create(D3D_SDK_VERSION);IDirect3DDevice9 *device=nullptr;
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr=api&&window?api->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device):E_FAIL;
    check(SUCCEEDED(hr)&&device,"create offscreen-capable D3D9 HAL device",hr);
    if(device)for(bool jpeg:{false,true}){
        std::wstring path=std::wstring(directory)+(jpeg?L"\\fixture.jpg":L"\\fixture.png");std::vector<BYTE> encoded,expected;
        check(file_bytes(path,encoded,false)&&file_bytes(path+L".bgra",expected,false),"read generated fixture and independent reference");
        if(encoded.empty()||expected.size()!=width*height*4)continue;
        D3DXIMAGE_INFO info{};hr=metadata(encoded.data(),(UINT)encoded.size(),&info);
        check(SUCCEEDED(hr)&&info.Width==width&&info.Height==height&&info.ImageFileFormat==(jpeg?D3DXIFF_JPG:D3DXIFF_PNG),jpeg?"D3DX decodes JPEG metadata":"D3DX decodes PNG metadata",hr);
        D3DXIMAGE_INFO bad{};HRESULT invalid=metadata(encoded.data(),jpeg?2:8,&bad);check(FAILED(invalid),"truncated image header is rejected",invalid);
        IDirect3DTexture9 *texture=nullptr;
        hr=load(device,encoded.data(),(UINT)encoded.size(),width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,&info,nullptr,&texture);
        check(SUCCEEDED(hr)&&texture,jpeg?"D3DX decodes JPEG into managed texture":"D3DX decodes alpha PNG into managed texture",hr);
        if(texture){
            // Native legacy JPEG decoders differ slightly from Windows WIC
            // even outside the prefix (observed max 11/255). Keep Wine/WIC
            // comparisons tight, and separately require exact GPU transfer.
            bool wine_helper=std::wstring(expected_helper).find(L"\\builtin\\")!=std::wstring::npos;
            D3DLOCKED_RECT lock{};hr=texture->LockRect(0,&lock,nullptr,D3DLOCK_READONLY);unsigned tolerance=jpeg?(wine_helper?2:16):0;
            check(SUCCEEDED(hr)&&close_pixels((BYTE*)lock.pBits,(UINT)lock.Pitch,expected,tolerance),"decoded texture matches independent reference within declared codec tolerance",hr);
            std::vector<BYTE> decoded;
            if(SUCCEEDED(hr)){
                if(lock.pBits&&lock.Pitch>=(int)width*4){decoded.resize(width*height*4);for(UINT y=0;y<height;y++)memcpy(decoded.data()+y*width*4,(BYTE*)lock.pBits+y*lock.Pitch,width*4);}
                texture->UnlockRect(0);
            }
            if(!decoded.empty())draw_and_read(device,texture,decoded,0);
            texture->Release();
        }
    }
    if(device)device->Release();if(api)api->Release();if(window)DestroyWindow(window);FreeLibrary(graphics);FreeLibrary(helper);
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    if(argc!=3&&argc!=5){fputs("d3dx-image-probe --generate DIRECTORY | --test DIRECTORY D3DX_DLL D3D9_DLL\n",stderr);return 2;}
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);check(SUCCEEDED(hr),"initialize COM",hr);if(FAILED(hr))return 1;
    if(argc==3&&!wcscmp(argv[1],L"--generate"))generate(argv[2]);
    else if(argc==5&&!wcscmp(argv[1],L"--test"))texture_tests(argv[2],argv[3],argv[4]);
    else{CoUninitialize();return 2;}
    CoUninitialize();printf("Image fixture %zu-bit: %d failure(s)\n",sizeof(void*)*8,failures);return failures?1:0;
}
