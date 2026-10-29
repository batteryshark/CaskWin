// SPDX-License-Identifier: LGPL-2.1-or-later
// Initial texture upload, cross-thread contexts, deferred commands and event
// completion. Only owned resources/windows; no input, capture or host settings.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <string>
#include <stdexcept>
#ifdef PREFER_DISCRETE
extern "C" {__declspec(dllexport) DWORD NvOptimusEnablement=1;__declspec(dllexport) int AmdPowerXpressRequestHighPerformance=1;}
#endif
template<class T>struct Com{T *p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->()const{return p;}};
static unsigned failures;
static const char *phase="startup";
static volatile LONG captured_exception;
static LONG WINAPI crash(EXCEPTION_POINTERS *info){
    HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress),&module);
    wchar_t name[1024]{};if(module)GetModuleFileNameW(module,name,1024);
    std::fprintf(stderr,"CRASH phase=%s code=%08lx address=%p module=%ls rva=%llx thread=%lu\n",phase,info->ExceptionRecord->ExceptionCode,info->ExceptionRecord->ExceptionAddress,name,static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(info->ExceptionRecord->ExceptionAddress)-reinterpret_cast<ULONG_PTR>(module)),GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}
static LONG CALLBACK observe_exception(EXCEPTION_POINTERS *info){
    if(info->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||InterlockedCompareExchange(&captured_exception,1,0))return EXCEPTION_CONTINUE_SEARCH;
    crash(info);
    wchar_t name[1024]{};DWORD length=GetEnvironmentVariableW(L"D3D11_THREAD_DUMP",name,1024);
    if(length&&length<1024){
        HANDLE file=CreateFileW(name,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(),info,FALSE};
            BOOL ok=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,
                static_cast<MINIDUMP_TYPE>(MiniDumpNormal|MiniDumpWithThreadInfo),&exception,nullptr,nullptr);
            std::fprintf(stderr,"FIXTURE_DUMP ok=%d error=%lu file=%ls\n",ok,ok?0:GetLastError(),name);CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void begin(const char *name){phase=name;std::printf("BEGIN phase=%s thread=%lu tick=%llu\n",phase,GetCurrentThreadId(),GetTickCount64());}
static void require(HRESULT hr,const char *operation){std::printf("API phase=%s op=%s hr=%08lx thread=%lu\n",phase,operation,ULONG(hr),GetCurrentThreadId());if(FAILED(hr))throw std::runtime_error(operation);}
static void check(bool ok,const char *operation){std::printf("%s phase=%s %s\n",ok?"PASS":"FAIL",phase,operation);if(!ok)++failures;}
static std::wstring path(HMODULE module){wchar_t text[1024];DWORD n=module?GetModuleFileNameW(module,text,1024):0;return n&&n<1024?std::wstring(text,n):L"";}
static HMODULE provider_object(void *object,bool wine,const char *name){
    HMODULE owner=nullptr;void *entry=object?(*static_cast<void***>(object))[0]:nullptr;
    if(entry)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(entry),&owner);
    std::wstring filename=path(owner);std::printf("OBJECT_PROVIDER name=%s entry=%p owner=%ls\n",name,entry,filename.c_str());
    check(!filename.empty()&&(filename.find(L"\\builtin\\")!=std::wstring::npos)==wine,name);
    return owner;
}
static void dxgi_imports(HMODULE module){
    auto base=reinterpret_cast<const BYTE*>(module);auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!rva)throw std::runtime_error("native D3D11 import directory missing");
    auto descriptor=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+rva);
    for(;descriptor->Name;++descriptor){
        if(_stricmp(reinterpret_cast<const char*>(base+descriptor->Name),"dxgi.dll"))continue;
        if(!descriptor->OriginalFirstThunk)throw std::runtime_error("native D3D11 DXGI import names absent");
        auto names=reinterpret_cast<const IMAGE_THUNK_DATA*>(base+descriptor->OriginalFirstThunk);
        auto addresses=reinterpret_cast<const IMAGE_THUNK_DATA*>(base+descriptor->FirstThunk);
        for(unsigned i=0;names[i].u1.AddressOfData;++i){
            if(IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))continue;
            auto name=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names[i].u1.AddressOfData);
            HMODULE owner=nullptr;auto target=reinterpret_cast<const void*>(addresses[i].u1.Function);
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(target),&owner);
            std::printf("NATIVE_D3D11_IAT name=%s target=%p owner=%ls\n",name->Name,target,path(owner).c_str());
        }
    }
}
static void backend_contract(){
    using Backend=BOOL(WINAPI *)(const void*,UINT,IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,
        const D3D_FEATURE_LEVEL*,UINT,UINT,const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,
        ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**,HRESULT*);
    HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
    FARPROC address=shim?GetProcAddress(shim,"WinedowsCreateSystemD3D11Device"):nullptr;
    Backend backend=nullptr;static_assert(sizeof(backend)==sizeof(address));std::memcpy(&backend,&address,sizeof(backend));
    if(!backend)throw std::runtime_error("native graphics backend export missing");
    begin("backend-contract");HRESULT result=0x12345678;SetLastError(0x2468);
    BOOL handled=backend(reinterpret_cast<const void*>(&backend_contract),0,nullptr,D3D_DRIVER_TYPE_HARDWARE,
        nullptr,0,nullptr,0,D3D11_SDK_VERSION,nullptr,nullptr,nullptr,nullptr,nullptr,&result);
    DWORD error=GetLastError();check(!handled&&result==0x12345678&&error==0x2468,"application caller declined with result and LastError intact");
    // Direct helper contract control: a supplied genuine system-code address
    // exercises classification/failure handling, not real driver provenance.
    // The independent NVIDIA call-stack and foreign-GL case provide that proof.
    const void *system_address=reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetTickCount64"));
    for(UINT entry=0;entry<2;++entry){
        ID3D11Device *device=nullptr;ID3D11DeviceContext *context=nullptr;IDXGISwapChain *swapchain=nullptr;
        result=S_OK;
        handled=backend(system_address,entry,nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,0xffffffff,
            nullptr,entry?&swapchain:nullptr,&device,nullptr,&context,&result);
        std::printf("BACKEND_CONTRACT entry=%u handled=%d hr=%08lx\n",entry,handled,ULONG(result));
        check(handled&&FAILED(result)&&!device&&!context&&!swapchain,"native SDK failure is final with empty outputs");
        if(device)device->Release();
        if(context)context->Release();
        if(swapchain)swapchain->Release();
    }
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;result=E_FAIL;
    handled=backend(system_address,0,nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        nullptr,nullptr,device.out(),nullptr,context.out(),&result);
    check(handled,"native backend handles explicit contract control");require(result,"native backend valid device");
    HMODULE owner=provider_object(device.p,false,"native backend device vtable belongs to native implementation");
    provider_object(context.p,false,"native backend immediate context vtable belongs to native implementation");
    dxgi_imports(owner);
}
static void drivers(){for(const wchar_t *name:{L"nvoglv32.dll",L"nvoglv64.dll",L"ig9icd32.dll",L"ig9icd64.dll",L"atio6axx.dll",L"atioglxx.dll"}){HMODULE module=GetModuleHandleW(name);if(module)std::printf("OPENGL_DRIVER %ls\n",path(module).c_str());}}
static void current_gl(const char *where){std::printf("WGL_CURRENT where=%s thread=%lu context=%p dc=%p\n",where,GetCurrentThreadId(),wglGetCurrentContext(),wglGetCurrentDC());}
struct ForeignGL {
    HWND window=nullptr;HDC dc=nullptr,previous_dc=nullptr;HGLRC context=nullptr,previous_context=nullptr;
    ForeignGL(){
        previous_context=wglGetCurrentContext();previous_dc=wglGetCurrentDC();
        window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Owned foreign GL context",WS_POPUP,0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(window)dc=GetDC(window);
        PIXELFORMATDESCRIPTOR format{};format.nSize=sizeof(format);format.nVersion=1;format.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;format.iPixelType=PFD_TYPE_RGBA;format.cColorBits=32;format.cDepthBits=24;
        int index=dc?ChoosePixelFormat(dc,&format):0;
        if(index&&SetPixelFormat(dc,index,&format))context=wglCreateContext(dc);
        if(!context||!wglMakeCurrent(dc,context))throw std::runtime_error("foreign GL setup");
    }
    ~ForeignGL(){wglMakeCurrent(previous_dc,previous_context);if(context)wglDeleteContext(context);if(dc)ReleaseDC(window,dc);if(window)DestroyWindow(window);}
    void verify(const char *operation){check(wglGetCurrentContext()==context&&wglGetCurrentDC()==dc,operation);}
};
static DWORD WINAPI watchdog(void*){Sleep(45000);std::fprintf(stderr,"TIMEOUT texture/thread fixture\n");TerminateProcess(GetCurrentProcess(),124);return 0;}
static void verify(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *texture,const std::vector<unsigned char> &expected){
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Com<ID3D11Texture2D> staging;require(device->CreateTexture2D(&desc,nullptr,staging.out()),"staging texture");
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT,0};Com<ID3D11Query> query;require(device->CreateQuery(&query_desc,query.out()),"event query");
    std::puts("COPY_AND_END");context->CopyResource(staging.p,texture);context->End(query.p);context->Flush();
    ULONGLONG start=GetTickCount64();BOOL complete=FALSE;HRESULT hr;unsigned polls=0;
    do{++polls;hr=context->GetData(query.p,&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(hr==S_OK&&complete)break;if(FAILED(hr))break;Sleep(1);}while(GetTickCount64()-start<10000);
    std::printf("QUERY phase=%s hr=%08lx complete=%d polls=%u elapsed_ms=%llu\n",phase,ULONG(hr),complete,polls,GetTickCount64()-start);
    require(hr,"event completion status");if(hr!=S_OK||!complete)throw std::runtime_error("event query timeout");
    D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped),"readback map");
    bool equal=true;for(unsigned y=0;y<desc.Height;++y)if(std::memcmp(static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch,expected.data()+y*desc.Width*4,desc.Width*4))equal=false;
    context->Unmap(staging.p,0);check(equal,"all 256 RGBA pixels match");
}
static void upload(ID3D11Device *device,ID3D11DeviceContext *context,const char *name){
    begin(name);std::vector<unsigned char> pixels(16*16*4);for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){unsigned i=(y*16+x)*4;pixels[i]=BYTE(x*17);pixels[i+1]=BYTE(y*17);pixels[i+2]=0x5a;pixels[i+3]=0xff;}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),64,1024};Com<ID3D11Texture2D> texture;
    require(device->CreateTexture2D(&desc,&data,texture.out()),"initial-data texture");Com<ID3D11ShaderResourceView> view;require(device->CreateShaderResourceView(texture.p,nullptr,view.out()),"shader resource view");
    drivers();verify(device,context,texture.p,pixels);
}
struct Worker{ID3D11Device *device;ID3D11DeviceContext *context;const char *name;bool ok=false;ID3D11RenderTargetView *rtv=nullptr;ID3D11CommandList *commands=nullptr;};
static DWORD WINAPI worker(void *value){
    auto args=static_cast<Worker*>(value);
    try{
        current_gl(args->name);
        if(!args->rtv)upload(args->device,args->context,args->name);
        else{begin(args->name);Com<ID3D11DeviceContext> deferred;require(args->device->CreateDeferredContext(0,deferred.out()),"deferred context");const float green[]{0,1,0,1};deferred->ClearRenderTargetView(args->rtv,green);require(deferred->FinishCommandList(FALSE,&args->commands),"finish command list");}
        args->ok=true;
    }catch(const std::exception &error){std::fprintf(stderr,"FAIL worker phase=%s %s\n",args->name,error.what());}
    return args->ok?0:1;
}
static void run_worker(Worker &args){DWORD tid=0;HANDLE thread=CreateThread(nullptr,0,worker,&args,0,&tid);if(!thread)throw std::runtime_error("CreateThread");std::printf("WORKER name=%s tid=%lu\n",args.name,tid);DWORD waited=WaitForSingleObject(thread,20000);CloseHandle(thread);if(waited!=WAIT_OBJECT_0){std::fprintf(stderr,"TIMEOUT worker phase=%s\n",args.name);TerminateProcess(GetCurrentProcess(),124);}if(!args.ok)throw std::runtime_error("worker operation");}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetUnhandledExceptionFilter(crash);AddVectoredExceptionHandler(1,observe_exception);HANDLE timer=CreateThread(nullptr,0,watchdog,nullptr,0,nullptr);if(timer)CloseHandle(timer);
    bool wine=false,with_swapchain=false,unbind=false,foreign=false,contract=false;for(int i=1;i<argc;++i){if(!wcscmp(argv[i],L"--wine"))wine=true;else if(!wcscmp(argv[i],L"--swapchain"))with_swapchain=true;else if(!wcscmp(argv[i],L"--unbind-before-workers"))unbind=true;else if(!wcscmp(argv[i],L"--foreign-context"))foreign=true;else if(!wcscmp(argv[i],L"--backend-contract"))contract=true;}
    HWND window=nullptr;
    try{
        begin("device");HMODULE library=LoadLibraryW(L"d3d11.dll");if(!library)throw std::runtime_error("load D3D11");
        decltype(&D3D11CreateDeviceAndSwapChain) create=nullptr;FARPROC address=GetProcAddress(library,"D3D11CreateDeviceAndSwapChain");static_assert(sizeof(create)==sizeof(address));std::memcpy(&create,&address,sizeof(create));if(!create)throw std::runtime_error("D3D11 factory");
        std::printf("PROVIDER %ls\n",path(library).c_str());bool builtin=path(library).find(L"\\builtin\\")!=std::wstring::npos;check(builtin==wine,"selected provider matches expectation");
        if(contract&&wine){backend_contract();begin("device");}
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        if(with_swapchain){window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Owned texture/thread fixture",WS_POPUP,20,30,128,96,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!window)throw std::runtime_error("owned window");ShowWindow(window,SW_SHOWNOACTIVATE);swap_desc.BufferDesc.Width=128;swap_desc.BufferDesc.Height=96;swap_desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;swap_desc.SampleDesc.Count=1;swap_desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap_desc.BufferCount=1;swap_desc.OutputWindow=window;swap_desc.Windowed=TRUE;swap_desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;}
        Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;Com<IDXGISwapChain> swap;D3D_FEATURE_LEVEL level{};const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
        require(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,4,D3D11_SDK_VERSION,with_swapchain?&swap_desc:nullptr,with_swapchain?swap.out():nullptr,device.out(),&level,context.out()),"hardware device");
        provider_object(device.p,wine,"D3D11 device vtable matches selected provider");
        provider_object(context.p,wine,"D3D11 immediate context vtable matches selected provider");
        Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};require(device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(dxgi.out())),"DXGI device");require(dxgi->GetAdapter(adapter.out()),"adapter");require(adapter->GetDesc(&adapter_desc),"adapter description");
        std::printf("ADAPTER %ls vendor=%04x device=%04x feature=%04x swapchain=%d\n",adapter_desc.Description,adapter_desc.VendorId,adapter_desc.DeviceId,level,with_swapchain);
        provider_object(adapter.p,wine,"DXGI adapter vtable matches selected provider");
        upload(device.p,context.p,"main-initial");current_gl("main-after-upload");
        // Diagnostic only: this explicit application-side WGL manipulation is
        // never used by ordinary passing regression cases or by the runtime.
        HGLRC saved_gl=wglGetCurrentContext();HDC saved_dc=wglGetCurrentDC();
        if(unbind&&saved_gl){BOOL ok=wglMakeCurrent(nullptr,nullptr);std::printf("DIAGNOSTIC_UNBIND ok=%d error=%lu\n",ok,ok?0:GetLastError());if(!ok)throw std::runtime_error("diagnostic WGL unbind");}
        Worker first{device.p,context.p,"worker-first"};run_worker(first);Worker second{device.p,context.p,"worker-after-exit"};run_worker(second);
        if(unbind&&saved_gl){BOOL ok=wglMakeCurrent(saved_dc,saved_gl);std::printf("DIAGNOSTIC_RESTORE ok=%d error=%lu\n",ok,ok?0:GetLastError());if(!ok)throw std::runtime_error("diagnostic WGL restore");}
        upload(device.p,context.p,"main-after-workers");
        begin("deferred-setup");D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        Com<ID3D11Texture2D> target;Com<ID3D11RenderTargetView> rtv;require(device->CreateTexture2D(&desc,nullptr,target.out()),"deferred target");require(device->CreateRenderTargetView(target.p,nullptr,rtv.out()),"render target view");
        Worker record{device.p,context.p,"deferred-record",false,rtv.p};run_worker(record);Com<ID3D11CommandList> commands;commands.p=record.commands;
        begin("deferred-execute");context->ExecuteCommandList(commands.p,FALSE);std::vector<unsigned char> green(16*16*4);for(unsigned i=0;i<256;++i){green[i*4+1]=255;green[i*4+3]=255;}verify(device.p,context.p,target.p,green);
        begin("cleanup");context->ClearState();context->Flush();
        if(foreign){
            begin("foreign-GL-preservation");ForeignGL application_gl;
            {
                Com<ID3D11Device> other_device;Com<ID3D11DeviceContext> other_context;
                require(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,4,D3D11_SDK_VERSION,nullptr,nullptr,other_device.out(),nullptr,other_context.out()),"device with application GL current");
                application_gl.verify("application GL context preserved after device creation");
                upload(other_device.p,other_context.p,"foreign-GL-upload");
                application_gl.verify("application GL context preserved after texture upload and readback");
            }
            application_gl.verify("application GL context preserved after device destruction");
        }
    }catch(const std::exception &error){std::fprintf(stderr,"FAIL phase=%s %s\n",phase,error.what());++failures;}
    if(window)DestroyWindow(window);
    std::printf("SUMMARY failures=%u noPresent=true inputOrCapture=false\n",failures);return failures?1:0;
}
