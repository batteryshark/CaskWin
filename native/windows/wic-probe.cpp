// SPDX-License-Identifier: LGPL-2.1-or-later
// Constructor diagnostics for the native WIC backend used by Wine D3DX.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

static int failures;
static void check(HRESULT hr,const char *name){printf("%s %s hr=%08lx\n",SUCCEEDED(hr)?"PASS":"FAIL",name,(unsigned long)hr);if(FAILED(hr))++failures;}
static std::vector<BYTE> bytes(const std::wstring &path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return {};
    LARGE_INTEGER size{};std::vector<BYTE> result;DWORD read=0;
    if(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<1024*1024){result.resize((size_t)size.QuadPart);if(!ReadFile(file,result.data(),(DWORD)result.size(),&read,nullptr)||read!=result.size())result.clear();}
    CloseHandle(file);return result;
}
static void decode(IWICImagingFactory *factory,IWICBitmapDecoder *decoder,const std::wstring &path){
    auto encoded=bytes(path),expected=bytes(path+L".bgra");
    check(!encoded.empty()&&expected.size()==16*16*4?S_OK:E_FAIL,"read generated WIC fixture/reference");if(encoded.empty()||expected.size()!=16*16*4)return;
    IWICStream *stream=nullptr;IWICBitmapFrameDecode *frame=nullptr;IWICFormatConverter *converter=nullptr;
    HRESULT hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(encoded.data(),(DWORD)encoded.size());
    if(SUCCEEDED(hr))hr=decoder->Initialize(stream,WICDecodeMetadataCacheOnLoad);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    UINT width=0,height=0;if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
    check(SUCCEEDED(hr)&&width==16&&height==16?S_OK:FAILED(hr)?hr:E_FAIL,"COM decoder reads actual image dimensions");
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(frame,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    std::vector<BYTE> actual(expected.size());
    if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,64,(UINT)actual.size(),actual.data());
    check(SUCCEEDED(hr)&&actual==expected?S_OK:FAILED(hr)?hr:E_FAIL,"COM decoder returns exact host-reference RGBA pixels");
    if(converter)converter->Release();if(frame)frame->Release();if(stream)stream->Release();
}
static void exercise(IWICImagingFactory *factory){
    IWICStream *stream=nullptr;HRESULT hr=factory->CreateStream(&stream);check(hr,"WIC memory stream");if(stream)stream->Release();
    for(const GUID *format:{&GUID_ContainerFormatPng,&GUID_ContainerFormatJpeg}){
        const char *name=IsEqualGUID(*format,GUID_ContainerFormatPng)?"PNG":"JPEG";
        IWICBitmapDecoder *decoder=nullptr;hr=factory->CreateDecoder(*format,nullptr,&decoder);check(hr,name);if(decoder)decoder->Release();
    }
}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);check(hr,"initialize COM");if(FAILED(hr))return 1;
    wchar_t system[MAX_PATH]{},wow[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);GetSystemWow64DirectoryW(wow,MAX_PATH);wprintf(L"System directory: %ls; WOW64 directory: %ls\n",system,wow);
    HMODULE codecs=LoadLibraryW(L"windowscodecs.dll");if(!codecs){check(HRESULT_FROM_WIN32(GetLastError()),"load native Windows codecs");CoUninitialize();return 1;}
    using Create=HRESULT(WINAPI*)(UINT,IWICImagingFactory**);
    auto create=(Create)GetProcAddress(codecs,"WICCreateImagingFactory_Proxy");IWICImagingFactory *factory=nullptr;
    hr=create?create(WINCODEC_SDK_VERSION,&factory):E_NOINTERFACE;check(hr,"WICCreateImagingFactory_Proxy used by Wine D3DX");
    if(factory){exercise(factory);factory->Release();factory=nullptr;}
    hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_IWICImagingFactory,(void**)&factory);check(hr,"COM WICImagingFactory");
    if(factory)exercise(factory);
    MULTI_QI interfaces[]={{&IID_IWICImagingFactory,nullptr,E_FAIL},{&IID_IUnknown,nullptr,E_FAIL},{&IID_IClassFactory,nullptr,E_FAIL}};
    hr=CoCreateInstanceEx(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,nullptr,3,interfaces);
    check(hr==CO_S_NOTALLINTERFACES&&interfaces[0].pItf&&interfaces[1].pItf&&interfaces[2].hr==E_NOINTERFACE&&!interfaces[2].pItf?S_OK:FAILED(hr)?hr:E_FAIL,"extended WIC factory activation preserves partial QI");
    for(auto &entry:interfaces)if(entry.pItf)entry.pItf->Release();
    for(const CLSID *clsid:{&CLSID_WICPngDecoder,&CLSID_WICJpegDecoder}){
        bool png=IsEqualCLSID(*clsid,CLSID_WICPngDecoder);IWICBitmapDecoder *decoder=nullptr;
        hr=CoCreateInstance(*clsid,nullptr,CLSCTX_INPROC_SERVER,IID_IWICBitmapDecoder,(void**)&decoder);check(hr,png?"COM PNG decoder":"COM JPEG decoder");
        if(decoder){if(factory&&argc==3&&!wcscmp(argv[1],L"--images"))decode(factory,decoder,std::wstring(argv[2])+(png?L"\\fixture.png":L"\\fixture.jpg"));decoder->Release();}
    }
    if(factory)factory->Release();
    FreeLibrary(codecs);CoUninitialize();return failures?1:0;
}
