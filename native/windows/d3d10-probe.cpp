// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d10_1.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

template<class T> struct Com {
    T *value=nullptr;
    ~Com(){reset();}
    void reset(){if(value){value->Release();value=nullptr;}}
    T *operator->()const{return value;}
    T **out(){reset();return &value;}
};
static unsigned failures;
static void check(bool ok,const char *name,HRESULT hr=S_OK){printf("%s %s hr=%08lx\n",ok?"PASS":"FAIL",name,(ULONG)hr);if(!ok)++failures;}
static void require(HRESULT hr,const char *name){check(SUCCEEDED(hr),name,hr);if(FAILED(hr))throw std::runtime_error(name);}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> path(32768);DWORD size=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return size&&size<path.size()?std::wstring(path.data(),size):std::wstring();}
static bool same_directory(const std::wstring &a,const std::wstring &b){return !_wcsicmp(a.substr(0,a.find_last_of(L"\\/")).c_str(),b.substr(0,b.find_last_of(L"\\/")).c_str());}
static void render(ID3D10Device *device,IDXGISwapChain *swap,UINT width,UINT height){
    Com<ID3D10Texture2D> back,staging;Com<ID3D10RenderTargetView> view;
    require(swap->GetBuffer(0,__uuidof(ID3D10Texture2D),(void**)back.out()),"obtain D3D10 swapchain backbuffer");
    require(device->CreateRenderTargetView(back.value,nullptr,view.out()),"create D3D10 render target");
    FLOAT color[]={48.f/255,96.f/255,192.f/255,1};device->ClearRenderTargetView(view.value,color);
    D3D10_TEXTURE2D_DESC description{};back->GetDesc(&description);description.Usage=D3D10_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D10_CPU_ACCESS_READ;description.MiscFlags=0;
    require(device->CreateTexture2D(&description,nullptr,staging.out()),"create D3D10 readback surface");
    device->CopyResource(staging.value,back.value);D3D10_MAPPED_TEXTURE2D mapped{};
    require(staging->Map(0,D3D10_MAP_READ,0,&mapped),"map completed D3D10 GPU operation");
    auto p=(const BYTE*)mapped.pData+(height/2)*mapped.RowPitch+(width/2)*4;
    DWORD pixel=DWORD(p[0])<<16|DWORD(p[1])<<8|p[2];printf("D3D10 readback %ux%u: %06lx\n",width,height,pixel);
    check(pixel==0x3060c0,"D3D10 rendered pixels match exactly");staging->Unmap(0);
    require(swap->Present(0,0),"present D3D10 windowed swapchain");
}
static DWORD WINAPI timeout(void*){Sleep(45000);fprintf(stderr,"FAIL D3D10 fixture timeout\n");TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,timeout,nullptr,0,nullptr));
    bool wine=false,version1=false;for(int i=1;i<argc;i++){if(!wcscmp(argv[i],L"--wine"))wine=true;else if(!wcscmp(argv[i],L"--10-1"))version1=true;}
    DEVMODEW before{};before.dmSize=sizeof(before);check(!!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before),"snapshot current display mode");
    HWND window=nullptr;
    try {
        HMODULE frontend=LoadLibraryW(version1?L"d3d10_1.dll":L"d3d10.dll"),dxgi=GetModuleHandleW(L"dxgi.dll"),backend=GetModuleHandleW(L"wined3d.dll");
        check(frontend&&dxgi,"load selected D3D10 family");if(!frontend||!dxgi)throw std::runtime_error("module load");
        printf("D3D10 version: %s\nD3D10 module: %ls\nDXGI module: %ls\n",version1?"10.1":"10.0",module_path(frontend).c_str(),module_path(dxgi).c_str());
        bool wine_backend=backend&&GetProcAddress(backend,"wined3d_get_renderer");
        bool builtin_frontend=wine_backend&&same_directory(module_path(frontend),module_path(backend));
        bool builtin_dxgi=wine_backend&&same_directory(module_path(dxgi),module_path(backend));
        check(wine?(builtin_frontend&&builtin_dxgi):(!builtin_frontend&&!builtin_dxgi),"D3D10/DXGI provider family matches request");
        window=CreateWindowExW(0,L"STATIC",L"Winedows D3D10 probe",WS_OVERLAPPEDWINDOW,40,40,256,192,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window)throw std::runtime_error("window");ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
        if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0))check(!!IsChild(GetDesktopWindow(),window),"D3D10 window is inside virtual desktop");
        DXGI_SWAP_CHAIN_DESC description{};description.BufferDesc.Width=128;description.BufferDesc.Height=96;description.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.SampleDesc.Count=1;description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;description.BufferCount=1;description.OutputWindow=window;description.Windowed=TRUE;description.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        Com<ID3D10Device> device;Com<IDXGISwapChain> swap;
        if(version1){
            auto create=(decltype(&D3D10CreateDeviceAndSwapChain1))GetProcAddress(frontend,"D3D10CreateDeviceAndSwapChain1");if(!create)throw std::runtime_error("missing D3D10.1 factory");
            ID3D10Device1 *created=nullptr;HRESULT result=create(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,0,D3D10_FEATURE_LEVEL_10_1,D3D10_1_SDK_VERSION,&description,swap.out(),&created);device.value=created;
            require(result,"create hardware D3D10.1 device and swapchain");check(created->GetFeatureLevel()==D3D10_FEATURE_LEVEL_10_1,"D3D10.1 feature level honored");
        }else{
            auto create=(decltype(&D3D10CreateDeviceAndSwapChain))GetProcAddress(frontend,"D3D10CreateDeviceAndSwapChain");if(!create)throw std::runtime_error("missing D3D10 factory");
            require(create(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,0,D3D10_SDK_VERSION,&description,swap.out(),device.out()),"create hardware D3D10 device and swapchain");
        }
        Com<IDXGIDevice> dxgi_device;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_description{};
        require(device->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi_device.out()),"query D3D10 DXGI device");require(dxgi_device->GetAdapter(adapter.out()),"query D3D10 selected adapter");require(adapter->GetDesc(&adapter_description),"read D3D10 hardware adapter descriptor");
        printf("D3D10 adapter: %ls vendor=%04x device=%04x\n",adapter_description.Description,adapter_description.VendorId,adapter_description.DeviceId);
        render(device.value,swap.value,128,96);require(swap->ResizeBuffers(1,160,120,DXGI_FORMAT_R8G8B8A8_UNORM,0),"resize D3D10 windowed swapchain");render(device.value,swap.value,160,120);
        device->ClearState();device->Flush();
    }catch(const std::exception &error){fprintf(stderr,"FAIL D3D10 probe exception: %s\n",error.what());++failures;}
    if(window)DestroyWindow(window);
    DEVMODEW after{};after.dmSize=sizeof(after);bool mode=EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)&&before.dmPelsWidth==after.dmPelsWidth&&before.dmPelsHeight==after.dmPelsHeight&&before.dmBitsPerPel==after.dmBitsPerPel&&before.dmDisplayFrequency==after.dmDisplayFrequency;
    check(mode,"display mode unchanged");printf("D3D10 failures: %u\n",failures);return failures?1:0;
}
