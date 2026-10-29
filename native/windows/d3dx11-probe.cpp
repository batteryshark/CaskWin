// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <algorithm>
#include <cstdint>

// Public D3DX11 ABI from include/d3dx11async.h and d3dx11tex.h. Only opaque
// null load-info/thread-pump arguments are used; no private layout is guessed.
struct ID3DX11ThreadPump;
struct D3DX11_IMAGE_LOAD_INFO;
extern "C" {
HRESULT WINAPI D3DX11CheckVersion(UINT,UINT);
HRESULT WINAPI D3DX11CompileFromMemory(const char*,SIZE_T,const char*,const D3D_SHADER_MACRO*,
        ID3DInclude*,const char*,const char*,UINT,UINT,ID3DX11ThreadPump*,ID3DBlob**,ID3DBlob**,HRESULT*);
HRESULT WINAPI D3DX11CreateTextureFromMemory(ID3D11Device*,const void*,SIZE_T,D3DX11_IMAGE_LOAD_INFO*,
        ID3DX11ThreadPump*,ID3D11Resource**,HRESULT*);
}
template<class T> struct Com {
    T *p=nullptr; ~Com(){if(p)p->Release();}
    T*operator->()const{return p;}
    T**out(){if(p){p->Release();p=nullptr;}return &p;}
};
static void demand(bool okay,const char *message)
{if(!okay)throw std::runtime_error(message);}
static void hrcheck(HRESULT hr,const char *message)
{if(FAILED(hr)){std::fprintf(stderr,"%s hr=%08lx\n",message,(ULONG)hr);throw std::runtime_error(message);}}
static std::wstring module_path(HMODULE module)
{std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(module,path.data(),(DWORD)path.size());return n&&n<path.size()?std::wstring(path.data(),n):std::wstring();}
static bool same_file(const std::wstring &left,const std::wstring &right)
{
    HANDLE a=CreateFileW(left.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(right.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    bool okay=a!=INVALID_HANDLE_VALUE&&b!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(a,&x)&&GetFileInformationByHandle(b,&y)
            &&x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);return okay;
}
#ifdef WINEDOWS_STATIC_D3DX11
static FARPROC import_address(const char *name)
{
    auto base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for(auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);rva&&desc->Name;desc++){
        if(_stricmp((char*)base+desc->Name,"d3dx11_43.dll")||!desc->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                &&!strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name,name))return (FARPROC)values->u1.Function;
    }
    return nullptr;
}
#endif
using Compile=decltype(&D3DX11CompileFromMemory);
using Texture=decltype(&D3DX11CreateTextureFromMemory);
static const char include_source[]="float4 remap(float4 c) { return c.bgra * float4(SCALE,1,0.25,1); }\n";
struct Include final:ID3DInclude {
    unsigned opened=0,closed=0;
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type,LPCSTR name,LPCVOID,LPCVOID *data,UINT *size)override{
        if(type!=D3D_INCLUDE_LOCAL||strcmp(name,"helper-test.h"))return E_FAIL;
        *data=include_source;*size=sizeof(include_source)-1;opened++;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data)override{if(data!=include_source)return E_FAIL;closed++;return S_OK;}
};
static const char vertex_source[]=
    "float4 main(uint id:SV_VertexID):SV_Position { float2 p=id==0?float2(-1,-1):"
    "(id==1?float2(-1,3):float2(3,-1)); return float4(p,0,1); }";
static const char pixel_source[]=
    "#include \"helper-test.h\"\nTexture2D image:register(t0);"
    "float4 main(float4 pos:SV_Position):SV_Target { return remap(image.Load(int3(int2(pos.xy),0))); }";
using Pixel=std::array<unsigned char,4>;
struct ImageData {std::vector<unsigned char> file;std::array<Pixel,16> pixels;};
static void word(std::vector<unsigned char> &out,size_t offset,uint32_t value)
{std::memcpy(out.data()+offset,&value,4);}
static ImageData dds(bool compressed)
{
    ImageData out;out.file.resize(128+(compressed?8:64));
    word(out.file,0,0x20534444);word(out.file,4,124);word(out.file,8,0x1007|(compressed?0x80000:8));
    word(out.file,12,4);word(out.file,16,4);word(out.file,20,compressed?8:16);word(out.file,28,1);
    word(out.file,76,32);word(out.file,80,compressed?4:0x41);word(out.file,108,0x1000);
    if(compressed){
        word(out.file,84,0x31545844);out.file[128]=0;out.file[129]=0xf8;out.file[130]=0xe0;out.file[131]=7;
        for(unsigned y=0;y<4;y++)out.file[132+y]=0xe4;
        const Pixel colors[]={Pixel{255,0,0,255},Pixel{0,255,0,255},Pixel{170,85,0,255},Pixel{85,170,0,255}};
        for(unsigned i=0;i<16;i++)out.pixels[i]=colors[i%4];
    }else{
        word(out.file,88,32);word(out.file,92,0xff);word(out.file,96,0xff00);word(out.file,100,0xff0000);word(out.file,104,0xff000000);
        for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
            unsigned i=y*4+x;out.pixels[i]={(unsigned char)(11+x*53),(unsigned char)(13+y*59),(unsigned char)(17+(x+y)*29),255};
            std::memcpy(out.file.data()+128+i*4,out.pixels[i].data(),4);
        }
    }
    return out;
}
static void texture_draw(ID3D11Device *device,ID3D11DeviceContext *context,Texture create,
        ID3D11VertexShader *vs,ID3D11PixelShader *ps,bool compressed)
{
    auto image=dds(compressed);
    Com<ID3D11Resource> loaded;HRESULT completed=E_PENDING;
    hrcheck(create(device,image.file.data(),image.file.size(),nullptr,nullptr,loaded.out(),&completed),"D3DX11 texture from memory");
    demand(completed==S_OK,"texture completion result");
    Com<ID3D11Texture2D> source;hrcheck(loaded->QueryInterface(IID_ID3D11Texture2D,(void**)source.out()),"loaded texture interface");
    D3D11_TEXTURE2D_DESC source_desc{};source->GetDesc(&source_desc);
    demand(source_desc.Width==4&&source_desc.Height==4&&source_desc.ArraySize==1,"loaded texture dimensions");
    demand(source_desc.Format==(compressed?DXGI_FORMAT_BC1_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM),"DDS format");
    Com<ID3D11ShaderResourceView> srv;hrcheck(device->CreateShaderResourceView(loaded.p,nullptr,srv.out()),"DDS shader view");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> target;hrcheck(device->CreateTexture2D(&desc,nullptr,target.out()),"render target texture");
    Com<ID3D11RenderTargetView> rtv;hrcheck(device->CreateRenderTargetView(target.p,nullptr,rtv.out()),"render target view");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> readback;hrcheck(device->CreateTexture2D(&desc,nullptr,readback.out()),"readback texture");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    Com<ID3D11RasterizerState> rs;hrcheck(device->CreateRasterizerState(&raster,rs.out()),"rasterizer");
    const float clear[]={1,0,1,1};context->ClearRenderTargetView(rtv.p,clear);
    D3D11_VIEWPORT viewport{0,0,4,4,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(rs.p);
    context->OMSetRenderTargets(1,&rtv.p,nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);context->PSSetShaderResources(0,1,&srv.p);
    context->Draw(3,0);context->CopyResource(readback.p,target.p);
    D3D11_MAPPED_SUBRESOURCE mapped{};hrcheck(context->Map(readback.p,0,D3D11_MAP_READ,0,&mapped),"completed GPU shader readback");
    bool matched=true;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        const auto &in=image.pixels[y*4+x];const int expected[]={int(in[2])*128/255,int(in[1]),(int(in[0])+2)/4,int(in[3])};
        auto actual=(unsigned char*)mapped.pData+y*mapped.RowPitch+x*4;
        for(unsigned channel=0;channel<4;channel++)if(std::abs(int(actual[channel])-expected[channel])>1){
            std::fprintf(stderr,"pixel %u,%u channel%u got%u expected%d\n",x,y,channel,actual[channel],expected[channel]);matched=false;
        }
    }
    context->Unmap(readback.p,0);context->ClearState();context->Flush();
    demand(matched,"independent shader pixel mismatch");
    std::printf("PASS %s helper texture / compiled shader / 16 GPU pixels\n",compressed?"BC1 DDS":"RGBA DDS");
}
static int run(int argc,wchar_t **argv)
{
    demand(argc==5,"usage: helper-path d3d11-path compiler-path wine|native");
    const bool wine=!wcscmp(argv[4],L"wine");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    hrcheck(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM initialization");
    HMODULE helper=nullptr;Compile compile;Texture texture;decltype(&D3DX11CheckVersion) version;
#ifdef WINEDOWS_STATIC_D3DX11
    compile=D3DX11CompileFromMemory;texture=D3DX11CreateTextureFromMemory;version=D3DX11CheckVersion;
    auto bound=import_address("D3DX11CompileFromMemory");demand(bound!=nullptr,"actual static helper import");
    demand(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)bound,&helper),"static helper module");
#else
    helper=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    demand(helper!=nullptr,"dynamic helper load");
    compile=(Compile)GetProcAddress(helper,"D3DX11CompileFromMemory");texture=(Texture)GetProcAddress(helper,"D3DX11CreateTextureFromMemory");
    version=(decltype(version))GetProcAddress(helper,"D3DX11CheckVersion");
#endif
    demand(compile&&texture&&version,"helper exports");demand(same_file(module_path(helper),argv[1]),"helper provider identity");
    std::wprintf(L"Helper=%ls\n",module_path(helper).c_str());
    HRESULT correct=version(D3D11_SDK_VERSION,43),wrong_helper=version(D3D11_SDK_VERSION,42),wrong_d3d=version(0,43);
    std::printf("Version SDK=%u exact=%08lx older-helper=%08lx wrong-D3D=%08lx\n",D3D11_SDK_VERSION,(ULONG)correct,(ULONG)wrong_helper,(ULONG)wrong_d3d);
    demand(correct==S_OK&&wrong_helper==E_FAIL&&wrong_d3d==E_FAIL,"helper version contract");
    demand(version(D3D11_SDK_VERSION,44)==E_FAIL&&version(D3D11_SDK_VERSION+1,43)==E_FAIL
            &&version(~0u,~0u)==E_FAIL,"helper version boundary cases");
    HMODULE graphics=LoadLibraryExW(argv[2],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    demand(graphics&&same_file(module_path(graphics),argv[2]),"D3D11 provider identity");
    auto create=(decltype(&D3D11CreateDevice))GetProcAddress(graphics,"D3D11CreateDevice");demand(create!=nullptr,"D3D11 create export");
    Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL obtained;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    hrcheck(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,3,D3D11_SDK_VERSION,device.out(),&obtained,context.out()),"hardware D3D11 device");
    Include include;const D3D_SHADER_MACRO defines[]={{"SCALE","0.5"},{nullptr,nullptr}};
    Com<ID3DBlob> vcode,pcode,errors,sm5;
    hrcheck(compile(vertex_source,sizeof(vertex_source)-1,"vertex.hlsl",nullptr,nullptr,"main","vs_4_0",0,0,nullptr,vcode.out(),errors.out(),nullptr),"D3DX11 vertex compile");
    hrcheck(compile(pixel_source,sizeof(pixel_source)-1,"pixel.hlsl",defines,&include,"main","ps_4_0",0,0,nullptr,pcode.out(),errors.out(),nullptr),"D3DX11 pixel compile");
    demand(include.opened==1&&include.closed==1,"include callbacks balanced");
    const char compute[]="RWTexture2D<uint> result:register(u0);[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){result[p.xy]=p.x+7;}";
    hrcheck(compile(compute,sizeof(compute)-1,"compute.hlsl",nullptr,nullptr,"main","cs_5_0",0,0,nullptr,sm5.out(),errors.out(),nullptr),"D3DX11 SM5 compile");
    const wchar_t *compiler_name=wine?L"d3dcompiler_47.dll":L"d3dcompiler_43.dll";
    HMODULE compiler=GetModuleHandleW(compiler_name);
    demand(compiler&&same_file(module_path(compiler),argv[3]),"compiler provider identity");
    const char bad[]="this is not valid HLSL";
    Com<ID3DBlob> invalid;
    demand(FAILED(compile(bad,sizeof(bad)-1,"invalid.hlsl",nullptr,nullptr,"main","ps_4_0",0,0,nullptr,invalid.out(),errors.out(),nullptr))
            &&!invalid.p&&errors.p&&errors->GetBufferSize(),"compiler error diagnostic");
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
    hrcheck(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,vs.out()),"GPU vertex shader");
    hrcheck(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,ps.out()),"GPU pixel shader");
    texture_draw(device.p,context.p,texture,vs.p,ps.p,false);
    texture_draw(device.p,context.p,texture,vs.p,ps.p,true);
    auto image=dds(false);Com<ID3D11Resource> rejected;
    demand(texture(nullptr,image.file.data(),image.file.size(),nullptr,nullptr,rejected.out(),nullptr)==E_INVALIDARG,"null device rejection");
    demand(FAILED(texture(device.p,"bad",3,nullptr,nullptr,rejected.out(),nullptr)),"malformed image rejection");
    std::puts("PASS D3DX11 helper version / SM4+SM5 compile / include diagnostics / textures");
    // Native SaveTexture/ThreadPump are outside this fixture. Wine's upstream
    // implementations are incomplete and are not advertised as validated here.
    context->ClearState();context->Flush();
    return 0;
}
int wmain(int argc,wchar_t **argv)
{
    try{return run(argc,argv);}catch(const std::exception &error){std::fprintf(stderr,"FAIL %s error=%lu\n",error.what(),GetLastError());return 1;}
}
