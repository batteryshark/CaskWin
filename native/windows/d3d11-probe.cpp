// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <dbghelp.h>
#include <GL/gl.h>
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
static bool wine;
static unsigned failures;
static decltype(&MiniDumpWriteDump) dump_writer;
static wchar_t dump_path[32768];
static LONG WINAPI crash(EXCEPTION_POINTERS *exception){
    if(dump_writer&&*dump_path){HANDLE file=CreateFileW(dump_path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file!=INVALID_HANDLE_VALUE){MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception,FALSE};dump_writer(GetCurrentProcess(),GetCurrentProcessId(),file,MiniDumpNormal,&info,nullptr,nullptr);CloseHandle(file);}}
    return EXCEPTION_EXECUTE_HANDLER;
}
static void check(bool ok,const char *name,HRESULT hr=S_OK){printf("%s %s hr=%08lx\n",ok?"PASS":"FAIL",name,(ULONG)hr);if(!ok)++failures;}
static void require(HRESULT hr,const char *name){check(SUCCEEDED(hr),name,hr);if(FAILED(hr))throw std::runtime_error(name);}
static std::wstring module_path(HMODULE module){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return n&&n<path.size()?std::wstring(path.data(),n):std::wstring();}
static bool same_directory(const std::wstring &a,const std::wstring &b){return !_wcsicmp(a.substr(0,a.find_last_of(L"\\/")).c_str(),b.substr(0,b.find_last_of(L"\\/")).c_str());}
static HMODULE object_module(void *object){
    HMODULE owner=nullptr;void **vtable=*(void***)object;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)vtable[0],&owner);return owner;
}
static HRESULT create_factory(HMODULE module,UINT entry,REFIID iid,void **factory){
    const char *names[]={"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"};
    auto proc=GetProcAddress(module,names[entry]);if(!proc)return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if(entry==2)return ((HRESULT(WINAPI*)(UINT,REFIID,void**))proc)(0,iid,factory);
    return ((HRESULT(WINAPI*)(REFIID,void**))proc)(iid,factory);
}
static void check_factories(HMODULE dxgi){
    using Bridge=BOOL(WINAPI*)(const void*,UINT,UINT,REFIID,void**,HRESULT*);
    HMODULE shim=GetModuleHandleW(sizeof(void*)==8?L"winedows64.dll":L"winedows32.dll");
    auto bridge=shim?(Bridge)GetProcAddress(shim,"WinedowsCreateSystemDXGIFactory"):nullptr;
    auto native_caller=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetCurrentProcessId");
    for(UINT entry=0;entry<3;entry++){
        Com<IDXGIFactory> selected;
        require(create_factory(dxgi,entry,__uuidof(IDXGIFactory),(void**)selected.out()),"application creates selected DXGI factory");
        HMODULE selected_owner=object_module(selected.value);
        printf("DXGI factory %u application implementation: %ls\n",entry,module_path(selected_owner).c_str());
        check(selected_owner==dxgi,"application factory stays with selected DXGI provider");
        if(!bridge)continue;
        HRESULT result=E_PENDING;void *untouched=(void*)1;SetLastError(4321);
        BOOL handled=bridge((const void*)&check_factories,entry,0,__uuidof(IDXGIFactory),&untouched,&result);
        check(!handled&&result==E_PENDING&&untouched==(void*)1&&GetLastError()==4321,"host callback declines application callers without side effects");
        Com<IDXGIFactory> host;
        handled=bridge((const void*)native_caller,entry,0,__uuidof(IDXGIFactory),(void**)host.out(),&result);
        require(handled?result:E_UNEXPECTED,"host callback creates native DXGI factory");
        HMODULE host_owner=object_module(host.value);
        printf("DXGI factory %u host implementation: %ls\n",entry,module_path(host_owner).c_str());
        check(!wine||host_owner!=dxgi,"host factory is distinct from Wine provider");
        const GUID unavailable={0x82e271d9,0x3f70,0x4bf5,{0x81,0xf3,0x24,0x93,0x74,0xc7,0xa1,0x12}};
        void *expected_output=(void*)1,*actual_output=(void*)1;
        HRESULT expected=create_factory(host_owner,entry,unavailable,&expected_output);
        result=S_OK;handled=bridge((const void*)native_caller,entry,0,unavailable,&actual_output,&result);
        check(handled&&FAILED(expected)&&result==expected&&actual_output==expected_output,"host callback preserves native unsupported-interface error");
    }
}
#ifdef WINEDOWS_STATIC_D3D11_IMPORT
static FARPROC imported(const char *module,const char *symbol){
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;if(!rva)return nullptr;
    for(auto descriptor=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);descriptor->Name;descriptor++){
        if(_stricmp((char*)base+descriptor->Name,module))continue;
        auto names=(IMAGE_THUNK_DATA*)(base+descriptor->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+descriptor->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)){
            auto entry=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((char*)entry->Name,symbol))return (FARPROC)values->u1.Function;
        }
    }
    return nullptr;
}
#endif
static bool hardware_gl(HWND window){
    HDC dc=GetDC(window);PIXELFORMATDESCRIPTOR pfd{sizeof(pfd),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
    int format=ChoosePixelFormat(dc,&pfd);HGLRC context=nullptr;bool hardware=false;
    if(format&&SetPixelFormat(dc,format,&pfd)&&(context=wglCreateContext(dc))&&wglMakeCurrent(dc,context)){
        auto renderer=(const char*)glGetString(GL_RENDERER),version=(const char*)glGetString(GL_VERSION);
        printf("Host OpenGL renderer: %s; version: %s\n",renderer?renderer:"unknown",version?version:"unknown");
        std::string name=renderer?renderer:"";for(auto &c:name)c=(char)tolower(c);
        hardware=!name.empty()&&name.find("gdi generic")==name.npos&&name.find("llvmpipe")==name.npos&&name.find("softpipe")==name.npos&&name.find("software")==name.npos&&name.find("basic render")==name.npos;
    }
    wglMakeCurrent(nullptr,nullptr);if(context)wglDeleteContext(context);ReleaseDC(window,dc);return hardware;
}
struct Vertex {float position[3],color[4];};
static const char shader_source[]=R"(
struct VSIn { float3 position : POSITION; float4 color : COLOR0; };
struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0; };
VSOut vs_main(VSIn input) { VSOut output; output.position=float4(input.position,1); output.color=input.color; return output; }
float4 ps_main(VSOut input) : SV_TARGET { return input.color; }
)";
static void render(ID3D11Device *device,ID3D11DeviceContext *context,IDXGISwapChain *swap,UINT width,UINT height){
    Com<ID3D11Texture2D> back,staging;Com<ID3D11RenderTargetView> view;
    require(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)back.out()),"obtain swapchain backbuffer");
    require(device->CreateRenderTargetView(back.value,nullptr,view.out()),"create render target");
    D3D11_VIEWPORT viewport{0,0,(FLOAT)width,(FLOAT)height,0,1};context->RSSetViewports(1,&viewport);
    context->OMSetRenderTargets(1,&view.value,nullptr);
    FLOAT background[]={16.f/255,32.f/255,48.f/255,1};context->ClearRenderTargetView(view.value,background);
    context->Draw(3,0);
    D3D11_TEXTURE2D_DESC description{};back->GetDesc(&description);description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;
    require(device->CreateTexture2D(&description,nullptr,staging.out()),"create GPU readback surface");
    context->CopyResource(staging.value,back.value);
    D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.value,0,D3D11_MAP_READ,0,&mapped),"map completed shader rendering");
    auto sample=[&](UINT x,UINT y){auto p=(const BYTE*)mapped.pData+y*mapped.RowPitch+x*4;return DWORD(p[0])<<16|DWORD(p[1])<<8|p[2];};
    DWORD interior=sample(width/2,height/2),exterior=sample(1,1);
    printf("Readback %ux%u: shader=%06lx background=%06lx\n",width,height,interior,exterior);
    check(interior==0x00ff00&&exterior==0x102030,"shader triangle and clear pixels match exactly");
    context->Unmap(staging.value,0);context->OMSetRenderTargets(0,nullptr,nullptr);
    require(swap->Present(0,0),"present rendered swapchain");
}
static DWORD WINAPI timeout(void*){Sleep(45000);fprintf(stderr,"FAIL D3D11 fixture timeout\n");TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);CloseHandle(CreateThread(nullptr,0,timeout,nullptr,0,nullptr));
    ULONG stack_guarantee=65536;SetThreadStackGuarantee(&stack_guarantee);
    if(GetEnvironmentVariableW(L"D3D11_PROBE_DUMP",dump_path,32768)){HMODULE dbghelp=LoadLibraryW(L"dbghelp.dll");dump_writer=dbghelp?(decltype(dump_writer))GetProcAddress(dbghelp,"MiniDumpWriteDump"):nullptr;SetUnhandledExceptionFilter(crash);}
    std::wstring preload;for(int i=1;i<argc;i++){if(!wcscmp(argv[i],L"--wine"))wine=true;else if(!wcscmp(argv[i],L"--native"))wine=false;else if(!wcscmp(argv[i],L"--preload-bundle")&&i+1<argc){preload=argv[++i];wine=true;}}
    DEVMODEW before{};before.dmSize=sizeof(before);check(!!EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before),"snapshot current display mode");
    HWND window=nullptr,gl_window=nullptr;
    try {
        if(!preload.empty())for(auto name:{L"wined3d.dll",L"dxgi.dll",L"d3d11.dll"}){
            HMODULE loaded=LoadLibraryW((preload+L"\\"+name).c_str());check(loaded!=nullptr,"explicit standalone Wine dependency preload",HRESULT_FROM_WIN32(loaded?0:GetLastError()));if(!loaded)throw std::runtime_error("preload");
        }
        HMODULE d3d11=LoadLibraryW(L"d3d11.dll"),dxgi=GetModuleHandleW(L"dxgi.dll"),wined3d=GetModuleHandleW(L"wined3d.dll");
        check(d3d11&&dxgi,"load selected D3D11 and DXGI");if(!d3d11||!dxgi)throw std::runtime_error("module load");
        printf("D3D11 module: %ls\nDXGI module: %ls\n",module_path(d3d11).c_str(),module_path(dxgi).c_str());
        if(wined3d)printf("WineD3D module: %ls\n",module_path(wined3d).c_str());
        bool wine_backend=wined3d&&GetProcAddress(wined3d,"wined3d_get_renderer");
        bool builtin_d3d11=wine_backend&&same_directory(module_path(d3d11),module_path(wined3d));
        bool builtin_dxgi=wine_backend&&same_directory(module_path(dxgi),module_path(wined3d));
        check(wine?(builtin_d3d11&&builtin_dxgi):(!builtin_d3d11&&!builtin_dxgi),"D3D11/DXGI provider family matches request");
        check_factories(dxgi);
        auto create=(decltype(&D3D11CreateDeviceAndSwapChain))GetProcAddress(d3d11,"D3D11CreateDeviceAndSwapChain");if(!create)throw std::runtime_error("missing D3D11 factory");
#ifdef WINEDOWS_STATIC_D3D11_IMPORT
        check(imported("d3d11.dll","D3D11CreateDeviceAndSwapChain")==reinterpret_cast<FARPROC>(create),"startup D3D11 import binds selected provider");
        create=D3D11CreateDeviceAndSwapChain;
#endif
        HINSTANCE instance=GetModuleHandleW(nullptr);
        window=CreateWindowExW(0,L"STATIC",L"Winedows D3D11 shader probe",WS_OVERLAPPEDWINDOW,40,40,256,192,nullptr,nullptr,instance,nullptr);
        if(!window)throw std::runtime_error("window");ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
        if(GetEnvironmentVariableW(L"WINEDOWS_DESKTOP_HWND",nullptr,0))check(!!IsChild(GetDesktopWindow(),window),"render window is inside virtual desktop");
        if(wine){gl_window=CreateWindowExW(0,L"STATIC",L"D3D11 hardware evidence",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,instance,nullptr);check(gl_window&&hardware_gl(gl_window),"host OpenGL renderer is hardware");}
        DXGI_SWAP_CHAIN_DESC swap_description{};swap_description.BufferDesc.Width=128;swap_description.BufferDesc.Height=96;swap_description.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;swap_description.SampleDesc.Count=1;swap_description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap_description.BufferCount=1;swap_description.OutputWindow=window;swap_description.Windowed=TRUE;swap_description.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};D3D_FEATURE_LEVEL feature{};
        Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;Com<IDXGISwapChain> swap;
        require(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,requested,3,D3D11_SDK_VERSION,&swap_description,swap.out(),device.out(),&feature,context.out()),"create hardware D3D11 device and swapchain");
        printf("D3D feature level: %04x\n",feature);check(feature>=D3D_FEATURE_LEVEL_10_0,"shader-model-4 feature level available");
        Com<IDXGIDevice> dxgi_device;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_description{};
        require(device->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi_device.out()),"query DXGI device");require(dxgi_device->GetAdapter(adapter.out()),"query selected adapter");require(adapter->GetDesc(&adapter_description),"read hardware adapter descriptor");
        printf("DXGI adapter: %ls vendor=%04x device=%04x VRAM=%llu\n",adapter_description.Description,adapter_description.VendorId,adapter_description.DeviceId,(unsigned long long)adapter_description.DedicatedVideoMemory);
        std::vector<wchar_t> system(32768);GetSystemDirectoryW(system.data(),(UINT)system.size());std::wstring compiler_path=std::wstring(system.data())+L"\\d3dcompiler_47.dll";HMODULE compiler=LoadLibraryW(compiler_path.c_str());
        if(!compiler)throw std::runtime_error("native shader compiler unavailable");printf("Shader compiler: %ls\n",module_path(compiler).c_str());
        auto compile=(decltype(&D3DCompile))GetProcAddress(compiler,"D3DCompile");if(!compile)throw std::runtime_error("shader compile export");
        Com<ID3DBlob> vertex_code,pixel_code,errors;
        HRESULT hr=compile(shader_source,sizeof(shader_source)-1,"d3d11-probe.hlsl",nullptr,nullptr,"vs_main","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,vertex_code.out(),errors.out());if(errors.value)printf("VS diagnostics: %s\n",(char*)errors->GetBufferPointer());require(hr,"compile real HLSL vertex shader");
        hr=compile(shader_source,sizeof(shader_source)-1,"d3d11-probe.hlsl",nullptr,nullptr,"ps_main","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,pixel_code.out(),errors.out());if(errors.value)printf("PS diagnostics: %s\n",(char*)errors->GetBufferPointer());require(hr,"compile real HLSL pixel shader");
        Com<ID3D11VertexShader> vertex;Com<ID3D11PixelShader> pixel;Com<ID3D11InputLayout> layout;Com<ID3D11Buffer> vertices;Com<ID3D11RasterizerState> rasterizer;
        require(device->CreateVertexShader(vertex_code->GetBufferPointer(),vertex_code->GetBufferSize(),nullptr,vertex.out()),"create executable vertex shader");require(device->CreatePixelShader(pixel_code->GetBufferPointer(),pixel_code->GetBufferSize(),nullptr,pixel.out()),"create executable pixel shader");
        D3D11_INPUT_ELEMENT_DESC input[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
        require(device->CreateInputLayout(input,2,vertex_code->GetBufferPointer(),vertex_code->GetBufferSize(),layout.out()),"create shader input layout");
        Vertex triangle[]={{{-.75f,-.75f,0},{0,1,0,1}},{{0,.75f,0},{0,1,0,1}},{{.75f,-.75f,0},{0,1,0,1}}};D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(triangle);buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA initial{triangle,0,0};
        require(device->CreateBuffer(&buffer,&initial,vertices.out()),"upload GPU triangle vertices");D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;require(device->CreateRasterizerState(&raster,rasterizer.out()),"create rasterizer state");
        UINT stride=sizeof(Vertex),offset=0;context->IASetVertexBuffers(0,1,&vertices.value,&stride,&offset);context->IASetInputLayout(layout.value);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.value,nullptr,0);context->PSSetShader(pixel.value,nullptr,0);context->RSSetState(rasterizer.value);
        render(device.value,context.value,swap.value,128,96);
        require(swap->ResizeBuffers(1,160,120,DXGI_FORMAT_R8G8B8A8_UNORM,0),"resize windowed swapchain");render(device.value,context.value,swap.value,160,120);
        Com<ID3D11VideoDevice> video;hr=device->QueryInterface(__uuidof(ID3D11VideoDevice),(void**)video.out());
        printf("VIDEO capability: ID3D11VideoDevice hr=%08lx decoder_profiles=%u; decoding not exercised\n",(ULONG)hr,video.value?video->GetVideoDecoderProfileCount():0);
        if(video.value){UINT count=video->GetVideoDecoderProfileCount();for(UINT i=0;i<count&&i<16;i++){GUID profile{};BOOL nv12=FALSE;if(SUCCEEDED(video->GetVideoDecoderProfile(i,&profile))){video->CheckVideoDecoderFormat(&profile,DXGI_FORMAT_NV12,&nv12);printf("VIDEO profile %u: %08lx-%04x-%04x NV12=%u\n",i,profile.Data1,profile.Data2,profile.Data3,nv12);}}}
        context->ClearState();context->Flush();swap.reset();context.reset();device.reset();errors.reset();pixel_code.reset();vertex_code.reset();FreeLibrary(compiler);
    }catch(const std::exception &error){fprintf(stderr,"FAIL D3D11 probe exception: %s\n",error.what());++failures;}
    if(gl_window)DestroyWindow(gl_window);if(window)DestroyWindow(window);
    DEVMODEW after{};after.dmSize=sizeof(after);bool mode=EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)&&before.dmPelsWidth==after.dmPelsWidth&&before.dmPelsHeight==after.dmPelsHeight&&before.dmBitsPerPel==after.dmBitsPerPel&&before.dmDisplayFrequency==after.dmDisplayFrequency;
    check(mode,"display mode unchanged");printf("D3D11 failures: %u\n",failures);return failures?1:0;
}
