// SPDX-License-Identifier: LGPL-2.1-or-later
// Independent DXGI/WineD3D Vulkan WSI lifecycle regression.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
#include "vendor/Detours-4.0.1/include/detours.h"
using Microsoft::WRL::ComPtr;
static void require(HRESULT hr,const char *name){printf("%s %s hr=%08lx\n",SUCCEEDED(hr)?"PASS":"FAIL",name,(ULONG)hr);if(FAILED(hr))throw std::runtime_error(name);}
static void check(bool ok,const char *name){require(ok?S_OK:E_FAIL,name);}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return n&&n<path.size()?std::wstring(path.data(),n):L"";}
using GetDeviceProc=FARPROC(WINAPI*)(void*,const char*);
using QueueSubmit=int(WINAPI*)(void*,UINT,const void*,UINT64);
using QueuePresent=int(WINAPI*)(void*,const void*);
static GetDeviceProc real_device_proc;
static GetDeviceProc real_instance_proc;
static QueueSubmit real_queue_submit;
static QueuePresent real_queue_present;
static volatile LONG submits,presents,present_errors;
static int WINAPI queue_submit(void *queue,UINT count,const void *info,UINT64 fence){int result=real_queue_submit(queue,count,info,fence);if(!result&&count)InterlockedIncrement(&submits);return result;}
static int WINAPI queue_present(void *queue,const void *info){int result=real_queue_present(queue,info);if(result>=0)InterlockedIncrement(&presents);else InterlockedIncrement(&present_errors);return result;}
static FARPROC WINAPI device_proc(void *device,const char *name){FARPROC proc=real_device_proc(device,name);if(proc&&!strcmp(name,"vkQueueSubmit")){real_queue_submit=(QueueSubmit)proc;return (FARPROC)queue_submit;}if(proc&&!strcmp(name,"vkQueuePresentKHR")){real_queue_present=(QueuePresent)proc;return (FARPROC)queue_present;}return proc;}
static FARPROC WINAPI instance_proc(void *instance,const char *name){FARPROC proc=real_instance_proc(instance,name);if(proc&&!strcmp(name,"vkGetDeviceProcAddr")){real_device_proc=(GetDeviceProc)proc;return (FARPROC)device_proc;}if(proc&&!strcmp(name,"vkQueueSubmit")){real_queue_submit=(QueueSubmit)proc;return (FARPROC)queue_submit;}if(proc&&!strcmp(name,"vkQueuePresentKHR")){real_queue_present=(QueuePresent)proc;return (FARPROC)queue_present;}return proc;}
struct VulkanEvidence {
    bool attached=false;HMODULE module=nullptr;
    explicit VulkanEvidence(bool wine){if(!wine)return;module=LoadLibraryW(L"winevulkan.dll");check(module!=nullptr,"load actual WineVulkan frontend");printf("WINE VULKAN MODULE %ls\n",module_path(module).c_str());real_instance_proc=(GetDeviceProc)GetProcAddress(module,"vkGetInstanceProcAddr");check(real_instance_proc!=nullptr,"resolve WineVulkan instance dispatch");check(DetourTransactionBegin()==NO_ERROR,"begin Vulkan call evidence");DetourUpdateThread(GetCurrentThread());check(DetourAttach((PVOID*)&real_instance_proc,instance_proc)==NO_ERROR,"instrument selected WineVulkan instance and device dispatch");check(DetourTransactionCommit()==NO_ERROR,"commit Vulkan call evidence");attached=true;}
    ~VulkanEvidence(){if(attached){DetourTransactionBegin();DetourUpdateThread(GetCurrentThread());DetourDetach((PVOID*)&real_instance_proc,instance_proc);DetourTransactionCommit();}if(module)FreeLibrary(module);}
};
static const char shader[]=R"(
struct V { float4 position : SV_Position; float4 color : COLOR; };
V vs(uint id : SV_VertexID) {
    V v; float2 p[3]={float2(-.75,-.75),float2(0,.75),float2(.75,-.75)};
    v.position=float4(p[id],0,1);v.color=float4(.125,.75,.25,1);return v;
}
float4 ps(V v):SV_Target { return v.color; }
)";
struct Pipeline {
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11RasterizerState> rasterizer;
    explicit Pipeline(ID3D11Device *device){
        wchar_t system[MAX_PATH]{};UINT size=GetSystemDirectoryW(system,MAX_PATH);check(size&&size<MAX_PATH,"resolve native shader compiler directory");auto path=std::wstring(system)+L"\\d3dcompiler_47.dll";HMODULE compiler=LoadLibraryW(path.c_str());check(compiler!=nullptr,"load shader compiler");auto compile=(decltype(&D3DCompile))GetProcAddress(compiler,"D3DCompile");check(compile!=nullptr,"resolve shader compiler");
        ComPtr<ID3DBlob> vs,ps,errors;require(compile(shader,sizeof(shader)-1,"wsi.hlsl",nullptr,nullptr,"vs","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors),"compile vertex shader");require(compile(shader,sizeof(shader)-1,"wsi.hlsl",nullptr,nullptr,"ps","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors),"compile pixel shader");require(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"create real vertex shader");require(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel),"create real pixel shader");vs.Reset();ps.Reset();errors.Reset();FreeLibrary(compiler);
        D3D11_RASTERIZER_DESC desc{};desc.FillMode=D3D11_FILL_SOLID;desc.CullMode=D3D11_CULL_NONE;desc.DepthClipEnable=TRUE;require(device->CreateRasterizerState(&desc,&rasterizer),"create deterministic rasterizer");
    }
    void bind(ID3D11DeviceContext *context){context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);context->RSSetState(rasterizer.Get());}
};
static void pump(){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
struct Window {
    HWND value=nullptr;
    Window(UINT width,UINT height,bool visible){value=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Winedows WineD3D Vulkan lifecycle",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,60,60,200,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(value!=nullptr,"create owned graphics fixture window");resize(width,height);if(visible)ShowWindow(value,SW_SHOWNOACTIVATE);pump();}
    void resize(UINT width,UINT height){RECT rect={0,0,(LONG)width,(LONG)height};check(!!AdjustWindowRectEx(&rect,(DWORD)GetWindowLongPtrW(value,GWL_STYLE),FALSE,(DWORD)GetWindowLongPtrW(value,GWL_EXSTYLE)),"calculate client geometry");check(!!SetWindowPos(value,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"resize owned fixture window");pump();RECT actual{};check(!!GetClientRect(value,&actual)&&actual.right==(LONG)width&&actual.bottom==(LONG)height,"owned window has requested client size");}
    ~Window(){if(value)DestroyWindow(value);}
};
static void draw(ID3D11Device *device,ID3D11DeviceContext *context,IDXGISwapChain *swap,const char *phase,bool present=true){
    ComPtr<ID3D11Texture2D> back,staging;ComPtr<ID3D11RenderTargetView> target;require(swap->GetBuffer(0,IID_PPV_ARGS(&back)),"obtain actual swapchain render buffer");D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);check(desc.Width>=16&&desc.Height>=16,"render buffer remains usable across WSI changes");require(device->CreateRenderTargetView(back.Get(),nullptr,&target),"bind real render buffer");D3D11_VIEWPORT viewport={0,0,(FLOAT)desc.Width,(FLOAT)desc.Height,0,1};context->RSSetViewports(1,&viewport);ID3D11RenderTargetView *view=target.Get();context->OMSetRenderTargets(1,&view,nullptr);const FLOAT color[]={.0625f,.125f,.25f,1};context->ClearRenderTargetView(view,color);context->Draw(3,0);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;require(device->CreateTexture2D(&desc,nullptr,&staging),"allocate independent shader readback");context->CopyResource(staging.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"map completed GPU shader pixels");
    UINT verified=0;for(UINT y=0;y<4;y++)for(UINT x=0;x<4;x++){UINT px=desc.Width/2-2+x,py=desc.Height/2-2+y;const BYTE *p=(const BYTE*)mapped.pData+py*mapped.RowPitch+px*4;check(abs((int)p[0]-32)<=1&&abs((int)p[1]-191)<=1&&abs((int)p[2]-64)<=1&&p[3]==255,"independent shader color matches");verified++;}
    const BYTE *background=(const BYTE*)mapped.pData+mapped.RowPitch+4;check(abs((int)background[0]-16)<=1&&abs((int)background[1]-32)<=1&&abs((int)background[2]-64)<=1,"clear pixel remains outside triangle");context->Unmap(staging.Get(),0);context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();back.Reset();
    HRESULT hr=present?swap->Present(0,0):S_OK;printf("WSI PHASE %s buffer=%ux%u shader_pixels=%u present=%08lx vk_submits=%ld vk_presents=%ld\n",phase,desc.Width,desc.Height,verified,(ULONG)hr,submits,presents);require(hr,"present accepts lifecycle state");pump();
}
static ComPtr<IDXGISwapChain> create_swapchain(IDXGIFactory *factory,ID3D11Device *device,HWND window){DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=128;desc.BufferDesc.Height=96;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;ComPtr<IDXGISwapChain> swap;require(factory->CreateSwapChain(device,&desc,&swap),"create explicit DXGI swapchain");return swap;}
static DWORD WINAPI watchdog(void*){Sleep(60000);puts("FAIL WineD3D Vulkan lifecycle watchdog");TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));bool wine=false,empty_first=false;UINT adapter_vendor=0;for(int i=1;i<argc;i++){wine|=!wcscmp(argv[i],L"--wine");empty_first|=!wcscmp(argv[i],L"--empty-first");if(!wcscmp(argv[i],L"--adapter")&&i+1<argc)adapter_vendor=wcstoul(argv[++i],nullptr,0);}
    DEVMODEW before{};before.dmSize=sizeof(before);if(!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before))return 2;int result=0;
    try{
        VulkanEvidence evidence(wine);HMODULE module=LoadLibraryW(L"d3d11.dll");check(module!=nullptr,"load selected D3D11 provider");printf("D3D11 MODULE %ls\n",module_path(module).c_str());auto create=(decltype(&D3D11CreateDevice))GetProcAddress(module,"D3D11CreateDevice");check(create!=nullptr,"resolve device-only factory");
        ComPtr<IDXGIAdapter> selected_adapter;HMODULE dxgi_module=nullptr;
        if(adapter_vendor){dxgi_module=LoadLibraryW(L"dxgi.dll");check(dxgi_module!=nullptr,"load adapter enumeration provider");auto create_factory=(HRESULT(WINAPI*)(REFIID,void**))GetProcAddress(dxgi_module,"CreateDXGIFactory");check(create_factory!=nullptr,"resolve adapter enumeration factory");ComPtr<IDXGIFactory> factory;require(create_factory(IID_PPV_ARGS(&factory)),"create adapter enumeration factory");for(UINT i=0;;i++){ComPtr<IDXGIAdapter> adapter;HRESULT hr=factory->EnumAdapters(i,&adapter);if(hr==DXGI_ERROR_NOT_FOUND)break;require(hr,"enumerate available adapter");DXGI_ADAPTER_DESC desc{};require(adapter->GetDesc(&desc),"describe enumerated adapter");printf("AVAILABLE ADAPTER index=%u vendor=%04x device=%04x name=%ls\n",i,desc.VendorId,desc.DeviceId,desc.Description);if(desc.VendorId==adapter_vendor&&!selected_adapter)selected_adapter=adapter;}check(selected_adapter!=nullptr,"requested hardware vendor is available");}
        for(UINT cycle=0;cycle<3;cycle++){
            ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};require(create(selected_adapter.Get(),selected_adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,3,D3D11_SDK_VERSION,&device,&level,&context),"create hardware device without application swapchain");
            HMODULE wined3d=GetModuleHandleW(L"wined3d.dll");auto renderer=wined3d?(int(__cdecl*)())GetProcAddress(wined3d,"wined3d_get_renderer"):nullptr;if(wine){check(renderer&&renderer()==1,"WineD3D selected Vulkan renderer");printf("WINED3D MODULE %ls renderer=%d\n",module_path(wined3d).c_str(),renderer());}else check(!renderer,"native comparison uses native D3D11");
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;require(device.As(&dxgi),"get graphics device adapter");require(dxgi->GetAdapter(&adapter),"get hardware graphics adapter");DXGI_ADAPTER_DESC desc{};require(adapter->GetDesc(&desc),"describe graphics adapter");printf("WSI ADAPTER %ls vendor=%04x device=%04x feature=%04x\n",desc.Description,desc.VendorId,desc.DeviceId,level);check(desc.VendorId!=0x1414,"graphics adapter is not Microsoft software rasterizer");if(adapter_vendor)check(desc.VendorId==adapter_vendor,"device uses explicitly selected hardware vendor");require(adapter->GetParent(IID_PPV_ARGS(&factory)),"get selected provider factory");require(factory->MakeWindowAssociation(nullptr,DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES),"disable fixture fullscreen transitions");
            Pipeline pipeline(device.Get());pipeline.bind(context.Get());Window window(empty_first?0:160,empty_first?0:120,!empty_first);if(!empty_first&&GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0))check(!!IsChild(GetDesktopWindow(),window.value),"graphics fixture belongs to virtual desktop");auto swap=create_swapchain(factory.Get(),device.Get(),window.value);
            draw(device.Get(),context.Get(),swap.Get(),empty_first?"created-empty":"created-visible");
            window.resize(208,144);ShowWindow(window.value,SW_SHOWNOACTIVATE);pump();if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0))check(!!IsChild(GetDesktopWindow(),window.value),"shown graphics fixture belongs to virtual desktop");draw(device.Get(),context.Get(),swap.Get(),"resized-window-old-buffer");draw(device.Get(),context.Get(),swap.Get(),"resized-window-second-present");
            require(swap->ResizeBuffers(1,208,144,DXGI_FORMAT_R8G8B8A8_UNORM,0),"resize buffers after WSI recreation");draw(device.Get(),context.Get(),swap.Get(),"resized-buffers");
            window.resize(160,0);draw(device.Get(),context.Get(),swap.Get(),"zero-height-window");window.resize(0,0);draw(device.Get(),context.Get(),swap.Get(),"empty-window");
            window.resize(192,128);ShowWindow(window.value,SW_SHOWMINNOACTIVE);pump();check(!!IsIconic(window.value),"fixture window is minimized");draw(device.Get(),context.Get(),swap.Get(),"minimized");
            ShowWindow(window.value,SW_SHOWNOACTIVATE);pump();if(IsIconic(window.value)){WINDOWPLACEMENT placement{};placement.length=sizeof(placement);check(!!GetWindowPlacement(window.value,&placement),"query minimized placement");placement.showCmd=SW_SHOWNOACTIVATE;check(!!SetWindowPlacement(window.value,&placement),"restore fixture without activation");pump();}check(!IsIconic(window.value),"fixture window restored");window.resize(192,128);require(swap->ResizeBuffers(1,0,0,DXGI_FORMAT_UNKNOWN,0),"recreate buffers from restored client size");draw(device.Get(),context.Get(),swap.Get(),"restored");
            context->ClearState();context->Flush();swap.Reset();pipeline.bind(context.Get());swap=create_swapchain(factory.Get(),device.Get(),window.value);draw(device.Get(),context.Get(),swap.Get(),"recreated-swapchain");context->ClearState();context->Flush();swap.Reset();printf("PASS complete device/swapchain lifecycle cycle=%u\n",cycle);
        }
        printf("VULKAN EVIDENCE submits=%ld presents=%ld recoverable_present_errors=%ld\n",submits,presents,present_errors);if(wine)check(submits>0&&presents>0,"real shader work and presentation traversed WineVulkan");selected_adapter.Reset();if(dxgi_module)FreeLibrary(dxgi_module);FreeLibrary(module);
    }catch(const std::exception &e){printf("FAIL WineD3D Vulkan lifecycle: %s\n",e.what());result=1;}
    DEVMODEW after{};after.dmSize=sizeof(after);if(!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)||before.dmPelsWidth!=after.dmPelsWidth||before.dmPelsHeight!=after.dmPelsHeight||before.dmBitsPerPel!=after.dmBitsPerPel||before.dmDisplayFrequency!=after.dmDisplayFrequency){puts("FAIL physical display mode changed");result=1;}else puts("PASS physical display mode unchanged");return result;
}
