// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
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
static std::wstring module_path(HMODULE module){wchar_t path[32768]{};DWORD size=GetModuleFileNameW(module,path,32768);return size&&size<32768?std::wstring(path,size):std::wstring();}
static bool same_file(const wchar_t *a,const wchar_t *b){
    HANDLE left=CreateFileW(a,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE right=CreateFileW(b,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};bool same=left!=INVALID_HANDLE_VALUE&&right!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(left,&x)&&GetFileInformationByHandle(right,&y)&&x.dwVolumeSerialNumber==y.dwVolumeSerialNumber&&x.nFileIndexHigh==y.nFileIndexHigh&&x.nFileIndexLow==y.nFileIndexLow;
    if(left!=INVALID_HANDLE_VALUE)CloseHandle(left);if(right!=INVALID_HANDLE_VALUE)CloseHandle(right);return same;
}
#ifdef WINEDOWS_STATIC_COMPILER47
static FARPROC startup_import(const char *name){
    BYTE *base=(BYTE*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS*)(base+dos->e_lfanew);
    DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;if(!rva)return nullptr;
    for(auto descriptor=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);descriptor->Name;++descriptor){
        if(_stricmp((const char*)base+descriptor->Name,"d3dcompiler_47.dll")||!descriptor->OriginalFirstThunk)continue;
        auto names=(IMAGE_THUNK_DATA*)(base+descriptor->OriginalFirstThunk),values=(IMAGE_THUNK_DATA*)(base+descriptor->FirstThunk);
        for(;names->u1.AddressOfData;++names,++values)if(!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)){
            auto imported=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if(!strcmp((const char*)imported->Name,name))return (FARPROC)values->u1.Function;
        }
    }
    return nullptr;
}
#endif
static const char common_source[]="float4 remap_color(float4 value, float4 tint) { return float4(value.bgr * tint.rgb, value.a * tint.a); }\n";
struct Include : ID3DInclude {
    unsigned opened=0,closed=0;
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type,LPCSTR name,LPCVOID,LPCVOID *data,UINT *size) override {
        if(type!=D3D_INCLUDE_LOCAL||strcmp(name,"shader-common.h"))return E_FAIL;
        *data=common_source;*size=sizeof(common_source)-1;++opened;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override {if(data!=common_source)return E_FAIL;++closed;return S_OK;}
};
static const char source[]=R"(
#include "shader-common.h"
cbuffer VertexParams : register(b1) { float4 scaleOffset; };
cbuffer PixelParams : register(b2) { float4 tint; uint mode; float3 padding; };
Texture2D image : register(t3);
SamplerState pointSampler : register(s2);
struct VertexInput { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput vertex_main(VertexInput input) {
    VertexOutput output;
#if APPLY_TRANSFORM
    output.position=float4(input.position * scaleOffset.xy + scaleOffset.zw,0,1);
#else
    output.position=float4(input.position,0,1);
#endif
    output.uv=input.uv; return output;
}
float4 pixel_main(VertexOutput input) : SV_TARGET {
    float4 value=image.Sample(pointSampler,input.uv);
    if(mode==1) return remap_color(value,tint);
    return value;
}
)";
static const char compute_source[]=R"(
RWBuffer<uint> outputData : register(u0);
[numthreads(4,1,1)] void compute_main(uint3 id : SV_DispatchThreadID) {
    outputData[id.x]=(id.x*7+3)^0x5a;
}
)";
struct Compiler {
    decltype(&D3DCompile) compile;
    decltype(&D3DCompile2) compile2;
    decltype(&D3DReflect) reflect;
    void shader(const char *text,const char *entry,const char *profile,bool second,Com<ID3DBlob> &code){
        Com<ID3DBlob> errors;Include include;D3D_SHADER_MACRO macros[]={{"APPLY_TRANSFORM","1"},{nullptr,nullptr}};
        UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr=second?compile2(text,strlen(text),"compiler47-probe.hlsl",macros,&include,entry,profile,flags,0,0,nullptr,0,code.out(),errors.out()):compile(text,strlen(text),"compiler47-probe.hlsl",macros,&include,entry,profile,flags,0,code.out(),errors.out());
        if(errors.value)printf("%s diagnostics: %s\n",profile,(char*)errors->GetBufferPointer());
        printf("Compiler target: %s; API: %s\n",profile,second?"D3DCompile2":"D3DCompile");require(hr,"compile HLSL through selected compiler");
        check(code.value&&code->GetBufferSize()>32&&!memcmp(code->GetBufferPointer(),"DXBC",4),"compiler emits a DXBC container");
        if(text==source)check(include.opened==1&&include.closed==1,"include callback is opened and closed exactly once");
    }
    void reflection(ID3DBlob *code,unsigned model,bool pixel){
        Com<ID3D11ShaderReflection> info;require(reflect(code->GetBufferPointer(),code->GetBufferSize(),IID_ID3D11ShaderReflection,(void**)info.out()),"reflect compiled shader");
        D3D11_SHADER_DESC desc{};require(info->GetDesc(&desc),"get reflected shader description");
        check(((desc.Version>>4)&15)==model&&(desc.Version&15)==0&&((desc.Version>>16)&0xffff)==(pixel?0u:1u),"reflection identifies stage and shader model");
        check(desc.InstructionCount>0,"reflection reports executable instructions");
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        require(info->GetResourceBindingDescByName(pixel?"PixelParams":"VertexParams",&binding),"reflect constant-buffer binding");
        check(binding.Type==D3D_SIT_CBUFFER&&binding.BindPoint==(pixel?2u:1u)&&binding.BindCount==1,"constant-buffer slot matches source contract");
        auto buffer=info->GetConstantBufferByName(pixel?"PixelParams":"VertexParams");D3D11_SHADER_BUFFER_DESC buffer_desc{};require(buffer->GetDesc(&buffer_desc),"reflect constant-buffer layout");
        check(buffer_desc.Size==(pixel?32u:16u),"constant-buffer byte size matches CPU layout");
        D3D11_SHADER_VARIABLE_DESC variable{};require(buffer->GetVariableByName(pixel?"tint":"scaleOffset")->GetDesc(&variable),"reflect vector constant");
        check(variable.StartOffset==0&&variable.Size==16,"vector constant layout matches CPU upload");
        if(pixel){
            require(buffer->GetVariableByName("mode")->GetDesc(&variable),"reflect dynamic branch constant");check(variable.StartOffset==16&&variable.Size==4,"branch constant has expected offset and size");
            require(info->GetResourceBindingDescByName("image",&binding),"reflect texture binding");check(binding.Type==D3D_SIT_TEXTURE&&binding.BindPoint==3&&binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D,"texture slot and dimension match source");
            require(info->GetResourceBindingDescByName("pointSampler",&binding),"reflect sampler binding");check(binding.Type==D3D_SIT_SAMPLER&&binding.BindPoint==2,"sampler slot matches source");
        }
    }
};
struct Vertex {float position[2],uv[2];};
static void render(ID3D11Device *device,ID3D11DeviceContext *context,Compiler &compiler,unsigned model){
    Com<ID3DBlob> vs_code,ps_code;compiler.shader(source,"vertex_main",model==4?"vs_4_0":"vs_5_0",model==5,vs_code);compiler.shader(source,"pixel_main",model==4?"ps_4_0":"ps_5_0",model==5,ps_code);
    compiler.reflection(vs_code.value,model,false);compiler.reflection(ps_code.value,model,true);
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;Com<ID3D11InputLayout> layout;
    require(device->CreateVertexShader(vs_code->GetBufferPointer(),vs_code->GetBufferSize(),nullptr,vs.out()),"D3D11 accepts compiled vertex shader");require(device->CreatePixelShader(ps_code->GetBufferPointer(),ps_code->GetBufferSize(),nullptr,ps.out()),"D3D11 accepts compiled pixel shader");
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    require(device->CreateInputLayout(elements,2,vs_code->GetBufferPointer(),vs_code->GetBufferSize(),layout.out()),"create reflected vertex input layout");
    Vertex vertices[]={{{-1,1},{0,0}},{{1,1},{1,0}},{{-1,-1},{0,1}},{{1,-1},{1,1}}};
    Com<ID3D11Buffer> vertex_buffer,vertex_constants,pixel_constants;D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(vertices);desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA initial{vertices,0,0};
    require(device->CreateBuffer(&desc,&initial,vertex_buffer.out()),"upload independent quad geometry");
    float transform[]={1,1,0,0};desc.ByteWidth=sizeof(transform);desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;initial.pSysMem=transform;require(device->CreateBuffer(&desc,&initial,vertex_constants.out()),"upload vertex constants");
    struct {float tint[4];UINT mode,padding[3];} constants={{1,.5f,.25f,1},1,{0,0,0}};
    desc.ByteWidth=sizeof(constants);desc.Usage=D3D11_USAGE_DEFAULT;initial.pSysMem=&constants;require(device->CreateBuffer(&desc,&initial,pixel_constants.out()),"upload pixel constants and dynamic branch selector");
    const BYTE pixels[]={128,0,0,255, 0,128,0,255, 0,0,128,255, 128,128,128,255};
    D3D11_TEXTURE2D_DESC texture{};texture.Width=texture.Height=2;texture.MipLevels=texture.ArraySize=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_IMMUTABLE;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> image,target,staging;Com<ID3D11ShaderResourceView> image_view;initial={pixels,8,0};require(device->CreateTexture2D(&texture,&initial,image.out()),"upload four independent reference texels");require(device->CreateShaderResourceView(image.value,nullptr,image_view.out()),"create sampled texture view");
    texture.Width=texture.Height=32;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_RENDER_TARGET;require(device->CreateTexture2D(&texture,nullptr,target.out()),"create offscreen GPU render target");
    Com<ID3D11RenderTargetView> target_view;require(device->CreateRenderTargetView(target.value,nullptr,target_view.out()),"create render target view");texture.Usage=D3D11_USAGE_STAGING;texture.BindFlags=0;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;require(device->CreateTexture2D(&texture,nullptr,staging.out()),"create shader output readback surface");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;Com<ID3D11SamplerState> sampler_state;require(device->CreateSamplerState(&sampler,sampler_state.out()),"create explicit point sampler");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;Com<ID3D11RasterizerState> raster_state;require(device->CreateRasterizerState(&raster,raster_state.out()),"create cull-free rasterizer");
    UINT stride=sizeof(Vertex),offset=0;D3D11_VIEWPORT viewport{0,0,32,32,0,1};context->IASetInputLayout(layout.value);context->IASetVertexBuffers(0,1,&vertex_buffer.value,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);context->VSSetShader(vs.value,nullptr,0);context->PSSetShader(ps.value,nullptr,0);context->VSSetConstantBuffers(1,1,&vertex_constants.value);context->PSSetConstantBuffers(2,1,&pixel_constants.value);context->PSSetShaderResources(3,1,&image_view.value);context->PSSetSamplers(2,1,&sampler_state.value);context->RSSetState(raster_state.value);context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&target_view.value,nullptr);
    // Expectations are CPU fixture values, independent of either shader compiler.
    const BYTE transformed[][4]={{0,0,32,255},{0,64,0,255},{128,0,0,255},{128,64,32,255}};
    for(unsigned branch=0;branch<2;branch++){
        constants.mode=branch;context->UpdateSubresource(pixel_constants.value,0,nullptr,&constants,0,0);float clear[]={1,0,1,1};context->ClearRenderTargetView(target_view.value,clear);context->Draw(4,0);context->CopyResource(staging.value,target.value);
        D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.value,0,D3D11_MAP_READ,0,&mapped),"read completed GPU shader pixels");bool exact=true;
        for(unsigned i=0;i<4;i++){auto actual=(const BYTE*)mapped.pData+(8+(i/2)*16)*mapped.RowPitch+(8+(i%2)*16)*4;auto expected=branch?transformed[i]:pixels+i*4;printf("SM%u branch%u texel%u: %u,%u,%u,%u expected %u,%u,%u,%u\n",model,branch,i,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);if(memcmp(actual,expected,4))exact=false;}
        context->Unmap(staging.value,0);check(exact,"all sampled shader pixels match independent expected bytes");
    }
    context->ClearState();context->Flush();
}
static void compute(ID3D11Device *device,ID3D11DeviceContext *context,Compiler &compiler){
    Com<ID3DBlob> code;compiler.shader(compute_source,"compute_main","cs_5_0",true,code);Com<ID3D11ShaderReflection> reflection;require(compiler.reflect(code->GetBufferPointer(),code->GetBufferSize(),IID_ID3D11ShaderReflection,(void**)reflection.out()),"reflect compute shader");
    UINT x=0,y=0,z=0;check(reflection->GetThreadGroupSize(&x,&y,&z)==4&&x==4&&y==1&&z==1,"compute reflection preserves thread-group dimensions");D3D11_SHADER_INPUT_BIND_DESC binding{};require(reflection->GetResourceBindingDescByName("outputData",&binding),"reflect typed UAV");check(binding.Type==D3D_SIT_UAV_RWTYPED&&binding.BindPoint==0,"compute reflection preserves UAV type and slot");
    Com<ID3D11ComputeShader> shader;require(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.out()),"D3D11 accepts compiled SM5 compute shader");
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Com<ID3D11Buffer> output,staging;require(device->CreateBuffer(&desc,nullptr,output.out()),"create compute output buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_UINT;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=4;Com<ID3D11UnorderedAccessView> uav;require(device->CreateUnorderedAccessView(output.value,&view,uav.out()),"create typed integer UAV");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;require(device->CreateBuffer(&desc,nullptr,staging.out()),"create compute result staging buffer");
    context->CSSetShader(shader.value,nullptr,0);context->CSSetUnorderedAccessViews(0,1,&uav.value,nullptr);context->Dispatch(1,1,1);context->CopyResource(staging.value,output.value);D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.value,0,D3D11_MAP_READ,0,&mapped),"read completed compute output");
    const UINT expected[]={0x59,0x50,0x4b,0x42};auto values=(const UINT*)mapped.pData;printf("SM5 compute bytes: %08x %08x %08x %08x\n",values[0],values[1],values[2],values[3]);check(!memcmp(values,expected,sizeof(expected)),"compute integer results match independent constants");context->Unmap(staging.value,0);context->ClearState();context->Flush();
}
static void structured_store_capability(Compiler &compiler){
    const char text[]="RWStructuredBuffer<uint> outputData : register(u0); [numthreads(4,1,1)] void main(uint3 id : SV_DispatchThreadID) { outputData[id.x]=(id.x*7+3)^0x5a; }";
    Com<ID3DBlob> code,errors;HRESULT hr=compiler.compile2(text,sizeof(text)-1,"structured-store.hlsl",nullptr,nullptr,"main","cs_5_0",0,0,0,nullptr,0,code.out(),errors.out());
    bool known_gap=hr==E_NOTIMPL&&errors.value&&strstr((const char*)errors->GetBufferPointer(),"Structured buffers store is not implemented");
    printf("CAPABILITY structured-buffer store: %s hr=%08lx; compile-only, not dispatched\n",SUCCEEDED(hr)?"compiled":known_gap?"unsupported upstream":"unexpected failure",(ULONG)hr);
    if(errors.value)printf("Structured-store diagnostics: %s\n",(const char*)errors->GetBufferPointer());
    check((SUCCEEDED(hr)&&code.value)||known_gap,"structured-store capability is reported without hiding unexpected errors");
}
static DWORD WINAPI watchdog(void*){Sleep(45000);fputs("FAIL compiler47 fixture timeout\n",stderr);TerminateProcess(GetCurrentProcess(),124);return 0;}
int wmain(int argc,wchar_t **argv){
    setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CloseHandle(CreateThread(nullptr,0,watchdog,nullptr,0,nullptr));
    if(argc!=3){fputs("compiler47-probe EXPECTED_COMPILER_DLL EXPECTED_D3D11_DLL\n",stderr);return 2;}
    try{
        HMODULE compiler_module=LoadLibraryW(L"d3dcompiler_47.dll"),graphics=LoadLibraryW(L"d3d11.dll");if(!compiler_module||!graphics)throw std::runtime_error("selected module unavailable");
        printf("Compiler provider: %ls\nGraphics provider: %ls\n",module_path(compiler_module).c_str(),module_path(graphics).c_str());check(same_file(module_path(compiler_module).c_str(),argv[1]),"compiler provider file identity matches selection");check(same_file(module_path(graphics).c_str(),argv[2]),"D3D11 provider file identity matches selection");
        Compiler compiler{(decltype(&D3DCompile))GetProcAddress(compiler_module,"D3DCompile"),(decltype(&D3DCompile2))GetProcAddress(compiler_module,"D3DCompile2"),(decltype(&D3DReflect))GetProcAddress(compiler_module,"D3DReflect")};
        if(!compiler.compile||!compiler.compile2||!compiler.reflect)throw std::runtime_error("compiler API missing");
#ifdef WINEDOWS_STATIC_COMPILER47
        check(startup_import("D3DCompile")==reinterpret_cast<FARPROC>(compiler.compile)&&startup_import("D3DCompile2")==reinterpret_cast<FARPROC>(compiler.compile2)&&startup_import("D3DReflect")==reinterpret_cast<FARPROC>(compiler.reflect),"startup compiler imports bind selected provider");compiler={D3DCompile,D3DCompile2,D3DReflect};
#endif
        Com<ID3DBlob> invalid,diagnostics;const char bad[]="float4 bad() : SV_TARGET { this is invalid HLSL; }";HRESULT hr=compiler.compile(bad,sizeof(bad)-1,"invalid.hlsl",nullptr,nullptr,"bad","ps_5_0",0,0,invalid.out(),diagnostics.out());check(FAILED(hr)&&!invalid.value&&diagnostics.value&&diagnostics->GetBufferSize()>1,"invalid HLSL fails with diagnostics and no executable shader");
        auto create=(decltype(&D3D11CreateDevice))GetProcAddress(graphics,"D3D11CreateDevice");if(!create)throw std::runtime_error("D3D11 factory missing");Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL feature{};const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};require(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,device.out(),&feature,context.out()),"create hardware D3D11 feature-level-11 device");
        Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};require(device->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi.out()),"query hardware DXGI device");require(dxgi->GetAdapter(adapter.out()),"query rendering adapter");require(adapter->GetDesc(&description),"read adapter identity");printf("Adapter: %ls vendor=%04x device=%04x feature=%04x\n",description.Description,description.VendorId,description.DeviceId,feature);check(description.VendorId!=0x1414,"hardware adapter is not Microsoft software rasterizer");
        render(device.value,context.value,compiler,4);render(device.value,context.value,compiler,5);compute(device.value,context.value,compiler);structured_store_capability(compiler);
    }catch(const std::exception &error){fprintf(stderr,"FAIL compiler47 exception: %s\n",error.what());++failures;}
    printf("Compiler47 %zu-bit failures: %u\n",sizeof(void*)*8,failures);return failures?1:0;
}
