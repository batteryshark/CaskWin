// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d10_1.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <algorithm>
#include <cstdint>

// Public D3DX10 ABI from include/d3dx10async.h and d3dx10tex.h. Only opaque
// null load-info/thread-pump arguments are used; no private layout is guessed.
struct ID3DX10ThreadPump;
struct D3DX10_IMAGE_LOAD_INFO;
struct D3DXMATRIX {float m[4][4];};
extern "C" {
D3DXMATRIX *WINAPI D3DXMatrixMultiply(D3DXMATRIX*,const D3DXMATRIX*,const D3DXMATRIX*);
D3DXMATRIX *WINAPI D3DXMatrixTranslation(D3DXMATRIX*,float,float,float);
D3DXMATRIX *WINAPI D3DXMatrixScaling(D3DXMATRIX*,float,float,float);
HRESULT WINAPI D3DX10CreateEffectFromMemory(const void*,SIZE_T,const char*,const D3D10_SHADER_MACRO*,
        ID3D10Include*,const char*,UINT,UINT,ID3D10Device*,ID3D10EffectPool*,ID3DX10ThreadPump*,ID3D10Effect**,ID3D10Blob**,HRESULT*);
HRESULT WINAPI D3DX10CheckVersion(UINT,UINT);
HRESULT WINAPI D3DX10CompileFromMemory(const char*,SIZE_T,const char*,const D3D_SHADER_MACRO*,
        ID3DInclude*,const char*,const char*,UINT,UINT,ID3DX10ThreadPump*,ID3DBlob**,ID3DBlob**,HRESULT*);
HRESULT WINAPI D3DX10CreateTextureFromMemory(ID3D10Device*,const void*,SIZE_T,D3DX10_IMAGE_LOAD_INFO*,
        ID3DX10ThreadPump*,ID3D10Resource**,HRESULT*);
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
#ifdef WINEDOWS_STATIC_D3DX10
static FARPROC import_address(const char *name)
{
    auto base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for(auto desc=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);rva&&desc->Name;desc++){
        if(_stricmp((char*)base+desc->Name,"d3dx10_43.dll")||!desc->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+desc->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;names++,values++)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                &&!strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData))->Name,name))return (FARPROC)values->u1.Function;
    }
    return nullptr;
}
#endif
using Compile=decltype(&D3DX10CompileFromMemory);
using Texture=decltype(&D3DX10CreateTextureFromMemory);
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
static void texture_draw(ID3D10Device *device,Texture create,
        ID3D10VertexShader *vs,ID3D10PixelShader *ps,ID3D10Effect *effect,bool compressed)
{
    auto image=dds(compressed);
    Com<ID3D10Resource> loaded;HRESULT completed=E_PENDING;
    hrcheck(create(device,image.file.data(),image.file.size(),nullptr,nullptr,loaded.out(),&completed),"D3DX10 texture from memory");
    demand(completed==S_OK,"texture completion result");
    Com<ID3D10Texture2D> source;hrcheck(loaded->QueryInterface(IID_ID3D10Texture2D,(void**)source.out()),"loaded texture interface");
    D3D10_TEXTURE2D_DESC source_desc{};source->GetDesc(&source_desc);
    demand(source_desc.Width==4&&source_desc.Height==4&&source_desc.ArraySize==1,"loaded texture dimensions");
    demand(source_desc.Format==(compressed?DXGI_FORMAT_BC1_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM),"DDS format");
    Com<ID3D10ShaderResourceView> srv;hrcheck(device->CreateShaderResourceView(loaded.p,nullptr,srv.out()),"DDS shader view");
    D3D10_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D10_USAGE_DEFAULT;desc.BindFlags=D3D10_BIND_RENDER_TARGET;
    Com<ID3D10Texture2D> target;hrcheck(device->CreateTexture2D(&desc,nullptr,target.out()),"render target texture");
    Com<ID3D10RenderTargetView> rtv;hrcheck(device->CreateRenderTargetView(target.p,nullptr,rtv.out()),"render target view");
    desc.Usage=D3D10_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
    Com<ID3D10Texture2D> readback;hrcheck(device->CreateTexture2D(&desc,nullptr,readback.out()),"readback texture");
    D3D10_RASTERIZER_DESC raster{};raster.FillMode=D3D10_FILL_SOLID;raster.CullMode=D3D10_CULL_NONE;raster.DepthClipEnable=TRUE;
    Com<ID3D10RasterizerState> rs;hrcheck(device->CreateRasterizerState(&raster,rs.out()),"rasterizer");
    const float clear[]={1,0,1,1};device->ClearRenderTargetView(rtv.p,clear);
    D3D10_VIEWPORT viewport{0,0,4,4,0,1};device->RSSetViewports(1,&viewport);device->RSSetState(rs.p);
    device->OMSetRenderTargets(1,&rtv.p,nullptr);device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(effect){
        float tint[]={0.25f,0.5f,0.75f,1.0f};
        auto color=effect->GetVariableByName("Tint")->AsVector();
        auto input=effect->GetVariableByName("image")->AsShaderResource();
        demand(color->IsValid()&&input->IsValid(),"effect parameters valid");
        hrcheck(color->SetFloatVector(tint),"effect uniform set");
        hrcheck(input->SetResource(srv.p),"effect resource set");
        auto pass=effect->GetTechniqueByName("Draw")->GetPassByIndex(0);demand(pass->IsValid(),"effect pass valid");
        hrcheck(pass->Apply(0),"effect pass apply");
    }else{device->VSSetShader(vs);device->PSSetShader(ps);device->PSSetShaderResources(0,1,&srv.p);}
    device->Draw(3,0);device->CopyResource(readback.p,target.p);
    D3D10_MAPPED_TEXTURE2D mapped{};hrcheck(readback->Map(0,D3D10_MAP_READ,0,&mapped),"completed GPU shader readback");
    bool matched=true;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){
        const auto &in=image.pixels[y*4+x];const int expected[]={effect?(int(in[2])+2)/4:int(in[2])*128/255, effect?(int(in[1])+1)/2:int(in[1]), effect?(int(in[0])*3+2)/4:(int(in[0])+2)/4,int(in[3])};
        auto actual=(unsigned char*)mapped.pData+y*mapped.RowPitch+x*4;
        for(unsigned channel=0;channel<4;channel++)if(std::abs(int(actual[channel])-expected[channel])>1){
            std::fprintf(stderr,"pixel %u,%u channel%u got%u expected%d\n",x,y,channel,actual[channel],expected[channel]);matched=false;
        }
    }
    readback->Unmap(0);device->ClearState();device->Flush();
    demand(matched,"independent shader pixel mismatch");
    std::printf("PASS %s helper texture / %s / 16 GPU pixels\n",compressed?"BC1 DDS":"RGBA DDS",effect?"effect pass + uniform":"compiled shader");
}
static const char effect_source[]=
    "Texture2D image; float4 Tint;"
    "float4 VS(uint id:SV_VertexID):SV_Position { float2 p=id==0?float2(-1,-1):"
    "(id==1?float2(-1,3):float2(3,-1)); return float4(p,0,1); }"
    "float4 PS(float4 p:SV_Position):SV_Target { return image.Load(int3(int2(p.xy),0)).bgra * Tint; }"
    "technique10 Draw { pass P0 { SetVertexShader(CompileShader(vs_4_0,VS()));"
    "SetGeometryShader(NULL); SetPixelShader(CompileShader(ps_4_0,PS())); } }";
static const char state_effect_source[]=
    "RasterizerState TestRaster { FillMode=Wireframe; CullMode=Front; DepthBias=7; };"
    "technique10 States { pass P0 { SetRasterizerState(TestRaster); } }";
static std::vector<unsigned char> read_effect(const wchar_t *path)
{
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    demand(file!=INVALID_HANDLE_VALUE,"open precompiled effect fixture");
    DWORD size=GetFileSize(file,nullptr),read=0;demand(size>=32&&size<1024*1024,"effect fixture bounded size");
    std::vector<unsigned char> data(size);bool okay=ReadFile(file,data.data(),size,&read,nullptr)&&read==size;CloseHandle(file);
    demand(okay&&!memcmp(data.data(),"DXBC",4),"precompiled effect fixture bytes");return data;
}
static int generate_effect(const wchar_t *path)
{
    // This is fixture generation only, outside Winedows. The tested Wine path
    // receives this independent native compiler's bytes as application input.
    wchar_t system[MAX_PATH];UINT count=GetSystemDirectoryW(system,MAX_PATH);demand(count&&count<MAX_PATH,"system directory");
    std::wstring compiler_path=std::wstring(system)+L"\\d3dcompiler_43.dll";
    HMODULE compiler=LoadLibraryExW(compiler_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    demand(compiler&&same_file(module_path(compiler),compiler_path),"fixture compiler identity");
    auto compile=(decltype(&D3DCompile))GetProcAddress(compiler,"D3DCompile");demand(compile!=nullptr,"fixture compiler export");
    Com<ID3DBlob> code,errors;
    HRESULT hr=compile(effect_source,sizeof(effect_source)-1,"draw.fx",nullptr,nullptr,nullptr,"fx_4_0",0,0,code.out(),errors.out());
    if(FAILED(hr)&&errors.p)std::fprintf(stderr,"%.*s\n",(int)errors->GetBufferSize(),(const char*)errors->GetBufferPointer());
    hrcheck(hr,"independent FX10 fixture compilation");
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);demand(file!=INVALID_HANDLE_VALUE,"create effect fixture");
    DWORD written=0;bool okay=WriteFile(file,code->GetBufferPointer(),(DWORD)code->GetBufferSize(),&written,nullptr)&&written==code->GetBufferSize();CloseHandle(file);
    demand(okay,"write effect fixture");std::printf("Generated independent FX10 fixture: %lu bytes\n",written);return 0;
}
static void math_check(HMODULE helper,const wchar_t *expected)
{
    (void)helper;
    decltype(&D3DXMatrixMultiply) multiply;decltype(&D3DXMatrixTranslation) translate;decltype(&D3DXMatrixScaling) scale;
    FARPROC bound;
#ifdef WINEDOWS_STATIC_D3DX10
    multiply=D3DXMatrixMultiply;translate=D3DXMatrixTranslation;scale=D3DXMatrixScaling;
    bound=import_address("D3DXMatrixMultiply");
#else
    multiply=(decltype(multiply))GetProcAddress(helper,"D3DXMatrixMultiply");
    translate=(decltype(translate))GetProcAddress(helper,"D3DXMatrixTranslation");
    scale=(decltype(scale))GetProcAddress(helper,"D3DXMatrixScaling");bound=(FARPROC)multiply;
#endif
    demand(multiply&&translate&&scale&&bound,"matrix helper exports");HMODULE provider=nullptr;
    demand(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)bound,&provider)&&same_file(module_path(provider),expected),"math provider identity");
    std::wprintf(L"Math=%ls\n",module_path(provider).c_str());
    D3DXMATRIX a{},b{},product{};demand(translate(&a,3,-5,7)==&a&&scale(&b,2,4,-3)==&b,"matrix return pointer");
    for(unsigned row=0;row<4;row++)for(unsigned col=0;col<4;col++){
        const float translation=row==3&&col<3?(col==0?3:col==1?-5:7):(row==col?1:0);
        const float scaling=row==col?(row==0?2:row==1?4:row==2?-3:1):0;
        demand(a.m[row][col]==translation&&b.m[row][col]==scaling,"independent matrix construction");
    }
    for(unsigned order=0;order<2;order++){
        auto &left=order?a:b;auto &right=order?b:a;
        demand(multiply(&product,&left,&right)==&product,"multiply return pointer");
        for(unsigned row=0;row<4;row++)for(unsigned col=0;col<4;col++){
            float expected_value=0;for(unsigned k=0;k<4;k++)expected_value+=left.m[row][k]*right.m[k][col];
            demand(product.m[row][col]==expected_value,"noncommutative matrix product");
        }
    }
    std::puts("PASS translation / scaling / both matrix multiplication orders");
}
static int run(int argc,wchar_t **argv)
{
    if(argc==3&&!wcscmp(argv[1],L"--generate-effect"))return generate_effect(argv[2]);
    demand(argc==7||argc==8,"usage: helper-path d3d10_1-path compiler-path math-path wine|native effect-file [--cpu-only]");
    const bool wine=!wcscmp(argv[5],L"wine");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    hrcheck(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM initialization");
    HMODULE helper=nullptr;Compile compile;Texture texture;decltype(&D3DX10CheckVersion) version;
    decltype(&D3DX10CreateEffectFromMemory) create_effect;
#ifdef WINEDOWS_STATIC_D3DX10
    compile=D3DX10CompileFromMemory;texture=D3DX10CreateTextureFromMemory;version=D3DX10CheckVersion;
    create_effect=D3DX10CreateEffectFromMemory;
    auto bound=import_address("D3DX10CompileFromMemory");demand(bound!=nullptr,"actual static helper import");
    demand(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)bound,&helper),"static helper module");
#else
    helper=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    demand(helper!=nullptr,"dynamic helper load");
    compile=(Compile)GetProcAddress(helper,"D3DX10CompileFromMemory");texture=(Texture)GetProcAddress(helper,"D3DX10CreateTextureFromMemory");
    version=(decltype(version))GetProcAddress(helper,"D3DX10CheckVersion");
    create_effect=(decltype(create_effect))GetProcAddress(helper,"D3DX10CreateEffectFromMemory");
#endif
    demand(compile&&texture&&version&&create_effect,"helper exports");demand(same_file(module_path(helper),argv[1]),"helper provider identity");
    std::wprintf(L"Helper=%ls\n",module_path(helper).c_str());
    std::printf("Version SDK=%u exact=%08lx older=%08lx wrong-D3D=%08lx\n",D3D10_SDK_VERSION,(ULONG)version(D3D10_SDK_VERSION,43),(ULONG)version(D3D10_SDK_VERSION,42),(ULONG)version(0,43));
    demand(version(D3D10_SDK_VERSION,43)==S_OK&&version(D3D10_SDK_VERSION,42)==E_FAIL&&version(0,43)==E_FAIL,"helper HRESULT version contract");
    demand(version(D3D10_SDK_VERSION,44)==E_FAIL&&version(D3D10_SDK_VERSION+1,43)==E_FAIL
            &&version(~0u,~0u)==E_FAIL,"helper version boundary cases");
    math_check(helper,argv[4]);
    Include include;const D3D_SHADER_MACRO defines[]={{"SCALE","0.5"},{nullptr,nullptr}};
    Com<ID3DBlob> vcode,pcode,errors;
    hrcheck(compile(vertex_source,sizeof(vertex_source)-1,"vertex.hlsl",nullptr,nullptr,"main","vs_4_0",0,0,nullptr,vcode.out(),errors.out(),nullptr),"D3DX10 vertex compile");
    hrcheck(compile(pixel_source,sizeof(pixel_source)-1,"pixel.hlsl",defines,&include,"main","ps_4_0",0,0,nullptr,pcode.out(),errors.out(),nullptr),"D3DX10 pixel compile");
    demand(include.opened==1&&include.closed==1,"include callbacks balanced");
    HMODULE compiler=GetModuleHandleW(wine?L"d3dcompiler_47.dll":L"d3dcompiler_43.dll");
    demand(compiler&&same_file(module_path(compiler),argv[3]),"compiler provider identity");
    std::wprintf(L"Compiler=%ls\n",module_path(compiler).c_str());
    const char bad[]="this is not valid HLSL";Com<ID3DBlob> invalid;
    demand(FAILED(compile(bad,sizeof(bad)-1,"invalid.hlsl",nullptr,nullptr,"main","ps_4_0",0,0,nullptr,invalid.out(),errors.out(),nullptr))
            &&!invalid.p&&errors.p&&errors->GetBufferSize(),"compiler error diagnostic");
    if(argc==8){demand(!wcscmp(argv[7],L"--cpu-only"),"unknown mode");std::puts("PASS D3DX10 CPU version / forwarded math / compiler");return 0;}
    HMODULE graphics=LoadLibraryExW(argv[2],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    demand(graphics&&same_file(module_path(graphics),argv[2]),"D3D10.1 provider identity");
    std::wprintf(L"Graphics=%ls\n",module_path(graphics).c_str());
    auto create=(decltype(&D3D10CreateDevice1))GetProcAddress(graphics,"D3D10CreateDevice1");demand(create!=nullptr,"D3D10.1 create export");
    Com<ID3D10Device1> device;
    hrcheck(create(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,0,D3D10_FEATURE_LEVEL_10_0,D3D10_1_SDK_VERSION,device.out()),"hardware D3D10 device");
    Com<ID3D10VertexShader> vs;Com<ID3D10PixelShader> ps;
    hrcheck(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),vs.out()),"GPU vertex shader");
    hrcheck(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),ps.out()),"GPU pixel shader");
    texture_draw(device.p,texture,vs.p,ps.p,nullptr,false);
    texture_draw(device.p,texture,vs.p,ps.p,nullptr,true);
    Com<ID3D10Effect> states,effect;
    HRESULT hr=create_effect(state_effect_source,sizeof(state_effect_source)-1,"states.fx",nullptr,nullptr,"fx_4_0",0,0,device.p,nullptr,nullptr,states.out(),errors.out(),nullptr);
    if(FAILED(hr)&&errors.p)std::fprintf(stderr,"Effect diagnostics: %.*s\n",(int)errors->GetBufferSize(),(const char*)errors->GetBufferPointer());
    hrcheck(hr,"D3DX10 state effect from source");demand(states.p!=nullptr,"source effect object");
    D3D10_EFFECT_DESC effect_desc{};hrcheck(states->GetDesc(&effect_desc),"source effect description");
    demand(effect_desc.Techniques==1,"source effect technique count");
    hrcheck(states->GetTechniqueByName("States")->GetPassByIndex(0)->Apply(0),"source effect pass apply");
    Com<ID3D10RasterizerState> current;device->RSGetState(current.out());demand(current.p!=nullptr,"source effect state bound");
    D3D10_RASTERIZER_DESC state_desc{};current->GetDesc(&state_desc);
    demand(state_desc.FillMode==D3D10_FILL_WIREFRAME&&state_desc.CullMode==D3D10_CULL_FRONT&&state_desc.DepthBias==7,"source-compiled effect raster state");
    device->ClearState();std::puts("PASS source-compiled effect / applied raster state");
    auto fx=read_effect(argv[6]);
    hrcheck(create_effect(fx.data(),fx.size(),"draw.fxo",nullptr,nullptr,nullptr,0,0,device.p,nullptr,nullptr,effect.out(),errors.out(),nullptr),"D3DX10 precompiled effect from memory");
    demand(effect.p!=nullptr,"precompiled effect object");hrcheck(effect->GetDesc(&effect_desc),"precompiled effect description");
    demand(effect_desc.Techniques==1&&effect_desc.ConstantBuffers==1,"precompiled effect description values");
    texture_draw(device.p,texture,nullptr,nullptr,effect.p,false);
    texture_draw(device.p,texture,nullptr,nullptr,effect.p,true);
    auto image=dds(false);Com<ID3D10Resource> rejected;
    demand(FAILED(texture(nullptr,image.file.data(),image.file.size(),nullptr,nullptr,rejected.out(),nullptr)),"null device rejection");
    demand(FAILED(texture(device.p,"bad",3,nullptr,nullptr,rejected.out(),nullptr)),"malformed image rejection");
    std::puts("PASS D3DX10 helper version / math / SM4 compile / effect / textures / 64 GPU pixels");
    device->ClearState();device->Flush();return 0;
}
int wmain(int argc,wchar_t **argv)
{
    try{return run(argc,argv);}catch(const std::exception &error){std::fprintf(stderr,"FAIL %s error=%lu\n",error.what(),GetLastError());return 1;}
}
